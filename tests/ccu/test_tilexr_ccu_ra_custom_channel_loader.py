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
LOADER_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.h"
LOADER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_loader.cpp"
TYPES_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_hccp_types.h"
ABI_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_abi_constants.h"
DIRECT_RUNTIME_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_direct_runtime.h"
DIRECT_RUNTIME_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_direct_runtime.cpp"
DRIVER_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.h"
DRIVER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp"


class TileXRCcuRaCustomChannelLoaderTest(unittest.TestCase):
    def setUp(self):
        lifecycle_hardening_tests = {
            "test_direct_runtime_ra_init_failure_report_keeps_phy_and_hdc_context",
            "test_direct_runtime_tlv_init_failure_report_keeps_ra_phy_and_hdc_context",
            "test_direct_runtime_uses_optional_tilexr_endpoint_route_provider_before_env",
        }
        runtime_source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")
        if (self._testMethodName in lifecycle_hardening_tests and
                "TILEXR_CCU_DIRECT_RESOURCE_WINDOW_EID_INDEX" not in runtime_source):
            self.skipTest("direct runtime lifecycle hardening is introduced by a later stack layer")

    def compile_and_run(self, code: str, extra_sources=None, extra_link_flags=None, extra_env=None):
        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        extra_sources = extra_sources or []
        extra_link_flags = extra_link_flags or []
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            test_cpp = temp_path / "hccp_loader_test.cpp"
            test_bin = temp_path / "hccp_loader_test"
            test_cpp.write_text(code, encoding="utf-8")
            subprocess.run(
                [
                    compiler,
                    "-std=c++14",
                    "-I",
                    str(REPO_ROOT / "src" / "include"),
                    "-I",
                    str(REPO_ROOT / "src" / "comm"),
                    str(test_cpp),
                    *[str(source) for source in extra_sources],
                    "-o",
                    str(test_bin),
                    *extra_link_flags,
                ],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            env = None if extra_env is None else {**extra_env}
            return subprocess.run(
                [str(test_bin)],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
                env=env,
            )

    def test_ccu_hccp_loader_exposes_ra_custom_channel_for_tilexr_ccu(self):
        header = LOADER_HEADER.read_text(encoding="utf-8")
        source = LOADER_SOURCE.read_text(encoding="utf-8")

        self.assertIn("TileXRCcuRaCustomChannelFunc", TYPES_HEADER.read_text(encoding="utf-8"))
        self.assertIn("RaCustomChannel", header)
        self.assertIn("RaCustomChannel = nullptr", header)
        self.assertIn('LoadSymbol(raHandle_, RaCustomChannel, "RaCustomChannel", "ra_custom_channel")', source)
        self.assertIn("RaCustomChannel = nullptr", source)

    def test_ccu_hccp_loader_exposes_runtime_phy_id_mapping_for_ccu_runtime(self):
        header = LOADER_HEADER.read_text(encoding="utf-8")
        source = LOADER_SOURCE.read_text(encoding="utf-8")

        self.assertIn("TileXRCcuRtGetDevicePhyIdByIndexFunc", TYPES_HEADER.read_text(encoding="utf-8"))
        self.assertIn("RtGetDevicePhyIdByIndex", header)
        self.assertIn("RtGetDevicePhyIdByIndex = nullptr", header)
        self.assertIn('dlopen("libruntime.so", RTLD_NOW)', source)
        self.assertIn(
            'LoadOptionalSymbol(runtimeHandle_, RtGetDevicePhyIdByIndex, "rtGetDevicePhyIdByIndex", nullptr)',
            source,
        )
        self.assertIn("ResolveDevicePhyId", header)
        self.assertIn("ResolveDevicePhyId", source)

    def test_ccu_hccp_loader_exposes_public_ra_ctx_resource_window_symbols(self):
        header = LOADER_HEADER.read_text(encoding="utf-8")
        source = LOADER_SOURCE.read_text(encoding="utf-8")
        types = TYPES_HEADER.read_text(encoding="utf-8")

        for needle in [
            "TileXRCcuRaGetDevEidInfoNumFunc",
            "TileXRCcuRaGetDevEidInfoListFunc",
            "TileXRCcuRaCtxInitFunc",
            "TileXRCcuRaCtxDeinitFunc",
            "TileXRCcuRaCtxTokenIdAllocFunc",
            "TileXRCcuRaCtxTokenIdFreeFunc",
            "TileXRCcuRaCtxLmemRegisterFunc",
            "TileXRCcuRaCtxLmemUnregisterFunc",
            "TileXRCcuRaGetSecRandomFunc",
            "TileXRCcuHccpMrRegInfo",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, types)

        for needle in [
            "RaGetDevEidInfoNum",
            "RaGetDevEidInfoList",
            "RaCtxInit",
            "RaCtxDeinit",
            "RaCtxTokenIdAlloc",
            "RaCtxTokenIdFree",
            "RaCtxLmemRegister",
            "RaCtxLmemUnregister",
            "RaGetSecRandom",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, header)
                self.assertIn(needle, source)

    def test_ccu_hccp_loader_exposes_public_ra_ctx_endpoint_route_symbols(self):
        header = LOADER_HEADER.read_text(encoding="utf-8")
        source = LOADER_SOURCE.read_text(encoding="utf-8")
        types = TYPES_HEADER.read_text(encoding="utf-8")

        for needle in [
            "TileXRCcuRaCtxCqCreateFunc",
            "TileXRCcuRaCtxCqDestroyFunc",
            "TileXRCcuRaCtxQpCreateFunc",
            "TileXRCcuRaCtxQpDestroyFunc",
            "TileXRCcuRaCtxQpImportFunc",
            "TileXRCcuRaCtxQpUnimportFunc",
            "TileXRCcuRaCtxQpBindFunc",
            "TileXRCcuRaCtxQpUnbindFunc",
            "TileXRCcuRaGetTpInfoListAsyncFunc",
            "TileXRCcuRaGetAsyncReqResultFunc",
            "TileXRCcuHccpQpCreateAttr",
            "TileXRCcuHccpQpImportInfo",
            "TileXRCcuHccpGetTpCfg",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, types)

        for needle in [
            "RaCtxCqCreate",
            "RaCtxCqDestroy",
            "RaCtxQpCreate",
            "RaCtxQpDestroy",
            "RaCtxQpImport",
            "RaCtxQpUnimport",
            "RaCtxQpBind",
            "RaCtxQpUnbind",
            "RaGetTpInfoListAsync",
            "RaGetAsyncReqResult",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, header)
                self.assertIn(needle, source)

    def test_ccu_hccp_loader_fails_closed_when_runtime_phy_id_mapping_fails(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #undef private

            #include <iostream>

            using namespace TileXR;

            int FakeRtGetDevicePhyIdByIndex(uint32_t logicDevId, uint32_t* phyId)
            {
                if (logicDevId != 7 || phyId == nullptr) {
                    return -99;
                }
                *phyId = 0xbeefU;
                return -1234;
            }

            int main()
            {
                TileXRCcuHccpLoader loader;
                loader.loaded_ = true;
                loader.RtGetDevicePhyIdByIndex = FakeRtGetDevicePhyIdByIndex;

                uint32_t phyId = 0;
                TileXRCcuHccpLoaderReport report;
                const int ret = loader.ResolveDevicePhyId(7, &phyId, &report);
                if (ret == TILEXR_SUCCESS) {
                    std::cerr << "phy id mapping failure silently succeeded\n";
                    return 1;
                }
                if (phyId == 7 || phyId == 0xbeefU) {
                    std::cerr << "failed mapping leaked fallback phy id: " << phyId << "\n";
                    return 2;
                }
                if (report.message.find("rtGetDevicePhyIdByIndex failed") == std::string::npos) {
                    std::cerr << "missing phy id failure message: " << report.message << "\n";
                    return 3;
                }
                if (report.logicDevId != 7 || report.runtimePhyIdRet != -1234) {
                    std::cerr << "phy id report mismatch\n";
                    return 4;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(code, extra_sources=[LOADER_SOURCE], extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_ccu_hccp_loader_initializes_and_deinitializes_ra_hdc_session(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #undef private

            #include <iostream>

            using namespace TileXR;

            TileXRCcuRaInitConfig g_lastInitConfig {};
            TileXRCcuRaInitConfig g_lastDeinitConfig {};
            int g_initCalls = 0;
            int g_deinitCalls = 0;
            int g_openCalls = 0;
            int g_closeCalls = 0;

            int FakeRtOpenNetService(const TileXRCcuRtNetServiceOpenArgs*)
            {
                ++g_openCalls;
                return 0;
            }

            int FakeRtCloseNetService()
            {
                ++g_closeCalls;
                return 0;
            }

            int FakeRaInit(TileXRCcuRaInitConfig* config)
            {
                if (config == nullptr) {
                    return -9;
                }
                g_lastInitConfig = *config;
                ++g_initCalls;
                return 0;
            }

            int FakeRaDeinit(TileXRCcuRaInitConfig* config)
            {
                if (config == nullptr) {
                    return -8;
                }
                g_lastDeinitConfig = *config;
                ++g_deinitCalls;
                return 0;
            }

            int main()
            {
                TileXRCcuHccpLoader first;
                first.loaded_ = true;
                first.RtOpenNetService = FakeRtOpenNetService;
                first.RtCloseNetService = FakeRtCloseNetService;
                first.RaInit = FakeRaInit;
                first.RaDeinit = FakeRaDeinit;

                TileXRCcuHccpLoader second;
                second.loaded_ = true;
                second.RtOpenNetService = FakeRtOpenNetService;
                second.RtCloseNetService = FakeRtCloseNetService;
                second.RaInit = FakeRaInit;
                second.RaDeinit = FakeRaDeinit;

                TileXRCcuHccpLoaderReport report;
                if (first.InitRaHdc(3, TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2, true, &report) != TILEXR_SUCCESS) {
                    std::cerr << "first init failed: " << report.message << "\n";
                    return 1;
                }
                if (second.InitRaHdc(3, TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2, true, &report) != TILEXR_SUCCESS) {
                    std::cerr << "second init failed: " << report.message << "\n";
                    return 2;
                }
                if (g_initCalls != 1 || !report.raInitialized || report.raInitRefCount != 2) {
                    std::cerr << "RA init refcount mismatch\n";
                    return 3;
                }
                if (g_openCalls != 1 || report.netServiceRefCount != 2) {
                    std::cerr << "net service refcount mismatch\n";
                    return 8;
                }
                if (g_lastInitConfig.phyId != 3 ||
                    g_lastInitConfig.nicPosition != TILEXR_CCU_NETWORK_OFFLINE ||
                    g_lastInitConfig.hdcType != TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2 ||
                    !g_lastInitConfig.enableHdcAsync) {
                    std::cerr << "RA init config mismatch\n";
                    return 4;
                }

                second.Unload();
                if (g_deinitCalls != 0) {
                    std::cerr << "RA deinit happened before last reference\n";
                    return 5;
                }
                if (g_closeCalls != 0) {
                    std::cerr << "runtime net service closed before last reference\n";
                    return 9;
                }
                first.Unload();
                if (g_deinitCalls != 1) {
                    std::cerr << "RA deinit count mismatch: " << g_deinitCalls << "\n";
                    return 6;
                }
                if (g_closeCalls != 1) {
                    std::cerr << "runtime net service close count mismatch: " << g_closeCalls << "\n";
                    return 10;
                }
                if (g_lastDeinitConfig.phyId != 3 ||
                    g_lastDeinitConfig.nicPosition != TILEXR_CCU_NETWORK_OFFLINE ||
                    g_lastDeinitConfig.hdcType != TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2) {
                    std::cerr << "RA deinit config mismatch\n";
                    return 7;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(code, extra_sources=[LOADER_SOURCE], extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_ccu_hccp_loader_opens_runtime_net_service_before_ra_init(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #undef private

            #include <cstring>
            #include <iostream>
            #include <string>

            using namespace TileXR;

            std::string g_openArg;
            int g_openCalls = 0;
            int g_closeCalls = 0;
            int g_initCalls = 0;
            int g_order = 0;
            int g_openOrder = 0;
            int g_initOrder = 0;

            int FakeRtOpenNetService(const TileXRCcuRtNetServiceOpenArgs* args)
            {
                ++g_openCalls;
                g_openOrder = ++g_order;
                if (args == nullptr || args->extParamList == nullptr || args->extParamCnt != 1 ||
                    args->extParamList[0].paramInfo == nullptr) {
                    return -11;
                }
                g_openArg.assign(args->extParamList[0].paramInfo, args->extParamList[0].paramLen);
                return 0;
            }

            int FakeRtCloseNetService()
            {
                ++g_closeCalls;
                return 0;
            }

            int FakeRaInit(TileXRCcuRaInitConfig*)
            {
                ++g_initCalls;
                g_initOrder = ++g_order;
                return 0;
            }

            int FakeRaDeinit(TileXRCcuRaInitConfig*)
            {
                return 0;
            }

            int main()
            {
                TileXRCcuHccpLoader loader;
                loader.loaded_ = true;
                loader.RtOpenNetService = FakeRtOpenNetService;
                loader.RtCloseNetService = FakeRtCloseNetService;
                loader.RaInit = FakeRaInit;
                loader.RaDeinit = FakeRaDeinit;

                TileXRCcuHccpLoaderReport report;
                if (loader.InitRaHdc(3, TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2, true, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed: " << report.message << "\n";
                    return 1;
                }
                if (g_openCalls != 1 || g_initCalls != 1 || g_openArg != "--hdcType=18") {
                    std::cerr << "open/init mismatch open=" << g_openCalls
                              << " init=" << g_initCalls
                              << " arg=" << g_openArg << "\n";
                    return 2;
                }
                if (g_openOrder == 0 || g_initOrder == 0 || g_openOrder > g_initOrder) {
                    std::cerr << "rtOpenNetService did not precede RaInit\n";
                    return 3;
                }
                loader.Unload();
                if (g_closeCalls != 1) {
                    std::cerr << "close count mismatch: " << g_closeCalls << "\n";
                    return 4;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(code, extra_sources=[LOADER_SOURCE], extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_ccu_hccp_loader_refcounts_runtime_net_service_by_hdc_type(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #undef private

            #include <iostream>

            using namespace TileXR;

            int g_openCalls = 0;
            int g_closeCalls = 0;
            int g_initCalls = 0;
            int g_deinitCalls = 0;

            int FakeRtOpenNetService(const TileXRCcuRtNetServiceOpenArgs*)
            {
                ++g_openCalls;
                return 0;
            }

            int FakeRtCloseNetService()
            {
                ++g_closeCalls;
                return 0;
            }

            int FakeRaInit(TileXRCcuRaInitConfig*)
            {
                ++g_initCalls;
                return 0;
            }

            int FakeRaDeinit(TileXRCcuRaInitConfig*)
            {
                ++g_deinitCalls;
                return 0;
            }

            int main()
            {
                TileXRCcuHccpLoader first;
                first.loaded_ = true;
                first.RtOpenNetService = FakeRtOpenNetService;
                first.RtCloseNetService = FakeRtCloseNetService;
                first.RaInit = FakeRaInit;
                first.RaDeinit = FakeRaDeinit;

                TileXRCcuHccpLoader second;
                second.loaded_ = true;
                second.RtOpenNetService = FakeRtOpenNetService;
                second.RtCloseNetService = FakeRtCloseNetService;
                second.RaInit = FakeRaInit;
                second.RaDeinit = FakeRaDeinit;

                TileXRCcuHccpLoaderReport report;
                if (first.InitRaHdc(3, TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2, true, &report) != TILEXR_SUCCESS ||
                    second.InitRaHdc(3, TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2, true, &report) != TILEXR_SUCCESS) {
                    std::cerr << "init failed: " << report.message << "\n";
                    return 1;
                }
                if (g_openCalls != 1 || g_initCalls != 1 || report.raInitRefCount != 2) {
                    std::cerr << "open/init ref mismatch open=" << g_openCalls
                              << " init=" << g_initCalls
                              << " refs=" << report.raInitRefCount << "\n";
                    return 2;
                }
                second.Unload();
                if (g_closeCalls != 0 || g_deinitCalls != 0) {
                    std::cerr << "closed/deinit before final reference\n";
                    return 3;
                }
                first.Unload();
                if (g_closeCalls != 1 || g_deinitCalls != 1) {
                    std::cerr << "final close/deinit mismatch close=" << g_closeCalls
                              << " deinit=" << g_deinitCalls << "\n";
                    return 4;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(code, extra_sources=[LOADER_SOURCE], extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_ccu_hccp_loader_fails_before_ra_init_when_net_service_open_fails(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #undef private

            #include <iostream>

            using namespace TileXR;

            int g_openCalls = 0;
            int g_initCalls = 0;

            int FakeRtOpenNetService(const TileXRCcuRtNetServiceOpenArgs*)
            {
                ++g_openCalls;
                return 128000;
            }

            int FakeRtCloseNetService()
            {
                return 0;
            }

            int FakeRaInit(TileXRCcuRaInitConfig*)
            {
                ++g_initCalls;
                return 0;
            }

            int FakeRaDeinit(TileXRCcuRaInitConfig*)
            {
                return 0;
            }

            int main()
            {
                TileXRCcuHccpLoader loader;
                loader.loaded_ = true;
                loader.RtOpenNetService = FakeRtOpenNetService;
                loader.RtCloseNetService = FakeRtCloseNetService;
                loader.RaInit = FakeRaInit;
                loader.RaDeinit = FakeRaDeinit;

                TileXRCcuHccpLoaderReport report;
                const int ret = loader.InitRaHdc(3, TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2, true, &report);
                if (ret == TILEXR_SUCCESS) {
                    std::cerr << "open failure unexpectedly succeeded\n";
                    return 1;
                }
                if (g_openCalls != 1 || g_initCalls != 0) {
                    std::cerr << "open failure call mismatch open=" << g_openCalls
                              << " init=" << g_initCalls << "\n";
                    return 2;
                }
                if (report.message.find("rtOpenNetService failed ret=128000") == std::string::npos ||
                    report.message.find("--hdcType=18") == std::string::npos) {
                    std::cerr << "missing open failure diagnostic: " << report.message << "\n";
                    return 3;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(code, extra_sources=[LOADER_SOURCE], extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_ccu_hccp_loader_initializes_and_releases_ccu_tlv_session(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #undef private

            #include <iostream>

            using namespace TileXR;

            TileXRCcuTlvInitInfo g_lastInitInfo {};
            uint32_t g_lastRequestModule = 0;
            uint32_t g_lastRequestType = 0;
            uint32_t g_lastReleaseType = 0;
            int g_initCalls = 0;
            int g_requestCalls = 0;
            int g_deinitCalls = 0;
            int g_order = 0;
            int g_initOrder = 0;
            int g_requestOrder = 0;
            int g_deinitOrder = 0;

            int FakeRaTlvInit(TileXRCcuTlvInitInfo* initInfo, uint32_t* bufferSize, void** tlvHandle)
            {
                ++g_initCalls;
                g_initOrder = ++g_order;
                if (initInfo == nullptr || bufferSize == nullptr || tlvHandle == nullptr) {
                    return -1;
                }
                g_lastInitInfo = *initInfo;
                *bufferSize = 4096;
                *tlvHandle = reinterpret_cast<void*>(0x12345678ULL);
                return 0;
            }

            int FakeRaTlvRequest(void* tlvHandle, uint32_t moduleType, TileXRCcuTlvMsg* sendMsg, TileXRCcuTlvMsg* recvMsg)
            {
                ++g_requestCalls;
                g_requestOrder = ++g_order;
                if (tlvHandle != reinterpret_cast<void*>(0x12345678ULL) || sendMsg == nullptr || recvMsg == nullptr) {
                    return -2;
                }
                g_lastRequestModule = moduleType;
                if (sendMsg->type == TILEXR_CCU_TLV_MSG_TYPE_CCU_UNINIT) {
                    g_lastReleaseType = sendMsg->type;
                } else {
                    g_lastRequestType = sendMsg->type;
                }
                return 0;
            }

            int FakeRaTlvDeinit(void* tlvHandle)
            {
                ++g_deinitCalls;
                g_deinitOrder = ++g_order;
                return tlvHandle == reinterpret_cast<void*>(0x12345678ULL) ? 0 : -3;
            }

            int main()
            {
                TileXRCcuHccpLoader loader;
                loader.loaded_ = true;
                loader.RaTlvInit = FakeRaTlvInit;
                loader.RaTlvRequest = FakeRaTlvRequest;
                loader.RaTlvDeinit = FakeRaTlvDeinit;

                TileXRCcuHccpLoaderReport report;
                if (loader.InitCcuTlv(3, &report) != TILEXR_SUCCESS) {
                    std::cerr << "tlv init failed: " << report.message << "\n";
                    return 1;
                }
                if (!report.ccuTlvInitialized || report.ccuTlvRefCount != 1 ||
                    report.ccuTlvBufferSize != 4096) {
                    std::cerr << "tlv report mismatch\n";
                    return 2;
                }
                if (g_initCalls != 1 || g_requestCalls != 1 || g_deinitCalls != 0) {
                    std::cerr << "tlv init call count mismatch\n";
                    return 3;
                }
                if (g_lastInitInfo.version != TILEXR_CCU_TLV_VERSION ||
                    g_lastInitInfo.phyId != 3 ||
                    g_lastInitInfo.nicPosition != TILEXR_CCU_NETWORK_OFFLINE ||
                    g_lastRequestModule != TILEXR_CCU_TLV_MODULE_TYPE_CCU ||
                    g_lastRequestType != TILEXR_CCU_TLV_MSG_TYPE_CCU_INIT) {
                    std::cerr << "tlv init/request envelope mismatch\n";
                    return 4;
                }
                if (g_initOrder == 0 || g_requestOrder == 0 || g_initOrder > g_requestOrder) {
                    std::cerr << "tlv request did not follow init\n";
                    return 5;
                }

                loader.Unload();
                if (g_requestCalls != 2 || g_lastReleaseType != TILEXR_CCU_TLV_MSG_TYPE_CCU_UNINIT ||
                    g_deinitCalls != 1) {
                    std::cerr << "tlv release mismatch requestCalls=" << g_requestCalls
                              << " releaseType=" << g_lastReleaseType
                              << " deinitCalls=" << g_deinitCalls << "\n";
                    return 6;
                }
                if (g_deinitOrder == 0 || g_requestOrder > g_deinitOrder) {
                    std::cerr << "tlv deinit happened before release request\n";
                    return 7;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(code, extra_sources=[LOADER_SOURCE], extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_ccu_hccp_loader_refcounts_ccu_tlv_by_device_phy_id(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #undef private

            #include <iostream>

            using namespace TileXR;

            int g_initCalls = 0;
            int g_requestCalls = 0;
            int g_deinitCalls = 0;

            int FakeRaTlvInit(TileXRCcuTlvInitInfo*, uint32_t* bufferSize, void** tlvHandle)
            {
                ++g_initCalls;
                *bufferSize = 1024;
                *tlvHandle = reinterpret_cast<void*>(0x1000ULL);
                return 0;
            }

            int FakeRaTlvRequest(void*, uint32_t, TileXRCcuTlvMsg*, TileXRCcuTlvMsg*)
            {
                ++g_requestCalls;
                return 0;
            }

            int FakeRaTlvDeinit(void*)
            {
                ++g_deinitCalls;
                return 0;
            }

            int main()
            {
                TileXRCcuHccpLoader first;
                first.loaded_ = true;
                first.RaTlvInit = FakeRaTlvInit;
                first.RaTlvRequest = FakeRaTlvRequest;
                first.RaTlvDeinit = FakeRaTlvDeinit;

                TileXRCcuHccpLoader second;
                second.loaded_ = true;
                second.RaTlvInit = FakeRaTlvInit;
                second.RaTlvRequest = FakeRaTlvRequest;
                second.RaTlvDeinit = FakeRaTlvDeinit;

                TileXRCcuHccpLoaderReport report;
                if (first.InitCcuTlv(3, &report) != TILEXR_SUCCESS ||
                    second.InitCcuTlv(3, &report) != TILEXR_SUCCESS) {
                    std::cerr << "tlv init failed: " << report.message << "\n";
                    return 1;
                }
                if (g_initCalls != 1 || g_requestCalls != 1 || report.ccuTlvRefCount != 2) {
                    std::cerr << "tlv refcount mismatch init=" << g_initCalls
                              << " request=" << g_requestCalls
                              << " refs=" << report.ccuTlvRefCount << "\n";
                    return 2;
                }
                second.Unload();
                if (g_deinitCalls != 0 || g_requestCalls != 1) {
                    std::cerr << "tlv released before final reference\n";
                    return 3;
                }
                first.Unload();
                if (g_deinitCalls != 1 || g_requestCalls != 2) {
                    std::cerr << "tlv final release mismatch deinit=" << g_deinitCalls
                              << " request=" << g_requestCalls << "\n";
                    return 4;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(code, extra_sources=[LOADER_SOURCE], extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_ccu_hccp_loader_rolls_back_tlv_handle_when_ccu_init_request_fails(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #undef private

            #include <iostream>

            using namespace TileXR;

            int g_initCalls = 0;
            int g_requestCalls = 0;
            int g_deinitCalls = 0;

            int FakeRaTlvInit(TileXRCcuTlvInitInfo*, uint32_t* bufferSize, void** tlvHandle)
            {
                ++g_initCalls;
                *bufferSize = 1024;
                *tlvHandle = reinterpret_cast<void*>(0x1000ULL);
                return 0;
            }

            int FakeRaTlvRequest(void*, uint32_t moduleType, TileXRCcuTlvMsg* sendMsg, TileXRCcuTlvMsg*)
            {
                ++g_requestCalls;
                if (moduleType != TILEXR_CCU_TLV_MODULE_TYPE_CCU ||
                    sendMsg == nullptr || sendMsg->type != TILEXR_CCU_TLV_MSG_TYPE_CCU_INIT) {
                    return -77;
                }
                return 128308;
            }

            int FakeRaTlvDeinit(void*)
            {
                ++g_deinitCalls;
                return 0;
            }

            int main()
            {
                TileXRCcuHccpLoader loader;
                loader.loaded_ = true;
                loader.RaTlvInit = FakeRaTlvInit;
                loader.RaTlvRequest = FakeRaTlvRequest;
                loader.RaTlvDeinit = FakeRaTlvDeinit;

                TileXRCcuHccpLoaderReport report;
                const int ret = loader.InitCcuTlv(3, &report);
                if (ret == TILEXR_SUCCESS) {
                    std::cerr << "tlv request failure unexpectedly succeeded\n";
                    return 1;
                }
                if (g_initCalls != 1 || g_requestCalls != 1 || g_deinitCalls != 1) {
                    std::cerr << "tlv rollback mismatch init=" << g_initCalls
                              << " request=" << g_requestCalls
                              << " deinit=" << g_deinitCalls << "\n";
                    return 2;
                }
                if (report.ccuTlvInitialized || report.raTlvRequestRet != 128308 ||
                    report.message.find("RaTlvRequest CCU_INIT failed ret=128308") == std::string::npos) {
                    std::cerr << "weak tlv failure diagnostic: " << report.message << "\n";
                    return 3;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(code, extra_sources=[LOADER_SOURCE], extra_link_flags=["-ldl"])

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_ccu_runtime_initializes_ra_hdc_after_resolving_device_phy_id(self):
        header = DIRECT_RUNTIME_HEADER.read_text(encoding="utf-8")
        source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")
        types = TYPES_HEADER.read_text(encoding="utf-8")
        loader_header = LOADER_HEADER.read_text(encoding="utf-8")
        loader_source = LOADER_SOURCE.read_text(encoding="utf-8")

        self.assertIn("TileXRCcuRaInitConfig", types)
        self.assertIn("TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2", types)
        self.assertIn("TileXRCcuRaInitFunc", types)
        self.assertIn("TileXRCcuRaDeinitFunc", types)
        self.assertIn("RaInit", loader_header)
        self.assertIn("RaDeinit", loader_header)
        self.assertIn("InitRaHdc", loader_header)
        self.assertIn('LoadSymbol(raHandle_, RaInit, "RaInit", nullptr)', loader_source)
        self.assertIn('LoadSymbol(raHandle_, RaDeinit, "RaDeinit", nullptr)', loader_source)
        self.assertIn("InitCcuTlv", loader_header)
        self.assertIn('LoadOptionalSymbol(raHandle_, RaTlvInit, "RaTlvInit", nullptr)', loader_source)
        self.assertIn('LoadOptionalSymbol(raHandle_, RaTlvRequest, "RaTlvRequest", nullptr)', loader_source)
        self.assertIn('LoadOptionalSymbol(raHandle_, RaTlvDeinit, "RaTlvDeinit", nullptr)', loader_source)
        self.assertIn("loader_.InitRaHdc", source)
        self.assertIn("loader_.InitRaHdc(devicePhyId_, hdcType, true, &raReport)", source)
        self.assertIn("loader_.InitCcuTlv(devicePhyId_, &tlvReport)", source)
        self.assertIn("SelectDirectCcuHdcType", source)
        self.assertIn("TILEXR_CCU_DIRECT_HDC_TYPE", source)
        self.assertIn("raInitialized", header)
        self.assertIn("ccuTlvInitialized", header)

    def test_direct_runtime_ra_init_failure_report_keeps_phy_and_hdc_context(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_runtime.h"

            #include <cstdlib>
            #include <iostream>

            using namespace TileXR;

            int main()
            {
                setenv("TILEXR_CCU_DIRECT_HDC_TYPE", "6", 1);

                TileXRCcuDirectRuntime runtime;
                TileXRCcuDirectRuntimeOptions options;
                options.devId = 7;
                options.rank = 0;
                options.rankSize = 2;
                TileXRCcuDirectRuntimeReport report;
                const int ret = runtime.Init(options, &report);
                if (ret == TILEXR_SUCCESS) {
                    std::cerr << "runtime init unexpectedly succeeded\n";
                    return 1;
                }
                if (report.logicDevId != 7 || report.devicePhyId != 0xbeefU ||
                    report.hdcType != TILEXR_CCU_HDC_SERVICE_TYPE_RDMA ||
                    report.raInitialized) {
                    std::cerr << "report lost init context logic=" << report.logicDevId
                              << " phy=" << report.devicePhyId
                              << " hdc=" << report.hdcType
                              << " ra=" << report.raInitialized << "\n";
                    return 2;
                }
                if (report.message.find("RaInit failed") == std::string::npos) {
                    std::cerr << "missing RA failure diagnostic: " << report.message << "\n";
                    return 3;
                }
                return 0;
            }
            '''
        )
        libra_code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_hccp_types.h"

            extern "C" int RaCustomChannel(
                TileXR::TileXRCcuRaInfo,
                TileXR::TileXRCcuCustomChannelIn*,
                TileXR::TileXRCcuCustomChannelOut*)
            {
                return 0;
            }

            extern "C" int RaInit(TileXR::TileXRCcuRaInitConfig*)
            {
                return -55;
            }

            extern "C" int RaDeinit(TileXR::TileXRCcuRaInitConfig*)
            {
                return 0;
            }
            '''
        )
        runtime_code = textwrap.dedent(
            r'''
            #include <cstdint>

            extern "C" int rtGetDevicePhyIdByIndex(uint32_t logicDevId, uint32_t* phyId)
            {
                if (logicDevId != 7 || phyId == nullptr) {
                    return -1;
                }
                *phyId = 0xbeefU;
                return 0;
            }

            struct TileXRCcuRtProcExtParam {
                const char *paramInfo;
                uint64_t paramLen;
            };

            struct TileXRCcuRtNetServiceOpenArgs {
                TileXRCcuRtProcExtParam *extParamList;
                uint64_t extParamCnt;
            };

            extern "C" int rtOpenNetService(const TileXRCcuRtNetServiceOpenArgs*)
            {
                return 0;
            }

            extern "C" int rtCloseNetService()
            {
                return 0;
            }
            '''
        )

        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            libra_cpp = temp_path / "fake_libra.cpp"
            runtime_cpp = temp_path / "fake_libruntime.cpp"
            libra_so = temp_path / "libra.so"
            runtime_so = temp_path / "libruntime.so"
            libra_cpp.write_text(libra_code, encoding="utf-8")
            runtime_cpp.write_text(runtime_code, encoding="utf-8")
            common_flags = [
                compiler,
                "-std=c++14",
                "-shared",
                "-fPIC",
                "-I",
                str(REPO_ROOT / "src" / "include"),
                "-I",
                str(REPO_ROOT / "src" / "comm"),
            ]
            subprocess.run(
                [*common_flags, str(libra_cpp), "-o", str(libra_so)],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            subprocess.run(
                [*common_flags, str(runtime_cpp), "-o", str(runtime_so)],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            result = self.compile_and_run(
                code,
                extra_sources=[
                    DIRECT_RUNTIME_SOURCE,
                    LOADER_SOURCE,
                    REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                    DRIVER_SOURCE,
                ],
                extra_link_flags=["-ldl"],
                extra_env={"LD_LIBRARY_PATH": str(temp_path)},
            )

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_runtime_tlv_init_failure_report_keeps_ra_phy_and_hdc_context(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_direct_runtime.h"

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                TileXRCcuDirectRuntimeOptions options;
                options.devId = 7;
                options.rank = 0;
                options.rankSize = 2;
                TileXRCcuDirectRuntimeReport report;
                const int ret = runtime.Init(options, &report);
                if (ret == TILEXR_SUCCESS) {
                    std::cerr << "runtime init unexpectedly succeeded\n";
                    return 1;
                }
                if (report.logicDevId != 7 || report.devicePhyId != 0xbeefU ||
                    report.hdcType != TILEXR_CCU_HDC_SERVICE_TYPE_RDMA_V2 ||
                    !report.raInitialized || report.ccuTlvInitialized) {
                    std::cerr << "report lost TLV failure context logic=" << report.logicDevId
                              << " phy=" << report.devicePhyId
                              << " hdc=" << report.hdcType
                              << " ra=" << report.raInitialized
                              << " tlv=" << report.ccuTlvInitialized << "\n";
                    return 2;
                }
                if (report.message.find("RaTlvRequest CCU_INIT failed ret=128308") == std::string::npos) {
                    std::cerr << "missing TLV failure diagnostic: " << report.message << "\n";
                    return 3;
                }
                return 0;
            }
            '''
        )
        libra_code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_hccp_types.h"

            extern "C" int RaCustomChannel(
                TileXR::TileXRCcuRaInfo,
                TileXR::TileXRCcuCustomChannelIn*,
                TileXR::TileXRCcuCustomChannelOut*)
            {
                return 0;
            }

            extern "C" int RaInit(TileXR::TileXRCcuRaInitConfig*)
            {
                return 0;
            }

            extern "C" int RaDeinit(TileXR::TileXRCcuRaInitConfig*)
            {
                return 0;
            }

            extern "C" int RaTlvInit(TileXR::TileXRCcuTlvInitInfo*, uint32_t* bufferSize, void** tlvHandle)
            {
                *bufferSize = 1024;
                *tlvHandle = reinterpret_cast<void*>(0x1000ULL);
                return 0;
            }

            extern "C" int RaTlvRequest(
                void*,
                uint32_t,
                TileXR::TileXRCcuTlvMsg* sendMsg,
                TileXR::TileXRCcuTlvMsg*)
            {
                return sendMsg != nullptr && sendMsg->type == TileXR::TILEXR_CCU_TLV_MSG_TYPE_CCU_INIT ?
                    128308 : 0;
            }

            extern "C" int RaTlvDeinit(void*)
            {
                return 0;
            }
            '''
        )
        runtime_code = textwrap.dedent(
            r'''
            #include <cstdint>

            extern "C" int rtGetDevicePhyIdByIndex(uint32_t logicDevId, uint32_t* phyId)
            {
                if (logicDevId != 7 || phyId == nullptr) {
                    return -1;
                }
                *phyId = 0xbeefU;
                return 0;
            }

            struct TileXRCcuRtProcExtParam {
                const char *paramInfo;
                uint64_t paramLen;
            };

            struct TileXRCcuRtNetServiceOpenArgs {
                TileXRCcuRtProcExtParam *extParamList;
                uint64_t extParamCnt;
            };

            extern "C" int rtOpenNetService(const TileXRCcuRtNetServiceOpenArgs*)
            {
                return 0;
            }

            extern "C" int rtCloseNetService()
            {
                return 0;
            }
            '''
        )

        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            libra_cpp = temp_path / "fake_libra.cpp"
            runtime_cpp = temp_path / "fake_libruntime.cpp"
            libra_so = temp_path / "libra.so"
            runtime_so = temp_path / "libruntime.so"
            libra_cpp.write_text(libra_code, encoding="utf-8")
            runtime_cpp.write_text(runtime_code, encoding="utf-8")
            common_flags = [
                compiler,
                "-std=c++14",
                "-shared",
                "-fPIC",
                "-I",
                str(REPO_ROOT / "src" / "include"),
                "-I",
                str(REPO_ROOT / "src" / "comm"),
            ]
            subprocess.run(
                [*common_flags, str(libra_cpp), "-o", str(libra_so)],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            subprocess.run(
                [*common_flags, str(runtime_cpp), "-o", str(runtime_so)],
                cwd=REPO_ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            result = self.compile_and_run(
                code,
                extra_sources=[
                    DIRECT_RUNTIME_SOURCE,
                    LOADER_SOURCE,
                    REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                    DRIVER_SOURCE,
                ],
                extra_link_flags=["-ldl"],
                extra_env={"LD_LIBRARY_PATH": str(temp_path)},
            )

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_ccu_driver_adapter_keeps_dynamic_loading_in_ccu_hccp_loader(self):
        combined = DRIVER_HEADER.read_text(encoding="utf-8") + "\n" + DRIVER_SOURCE.read_text(encoding="utf-8")

        for needle in [
            "dlopen",
            "dlsym",
            "libra.so",
            "libhcomm",
            "libhccl_v2",
            "#include <hcomm/",
            "#include <hccl/",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)

    def test_direct_ccu_runtime_queries_basic_info_through_ccu_hccp_loader(self):
        header = DIRECT_RUNTIME_HEADER.read_text(encoding="utf-8")
        source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")

        self.assertIn("tilexr_ccu_hccp_loader.h", header)
        self.assertIn("QueryBasicInfo", header)
        self.assertIn("CreateDriverAdapter", header)
        self.assertIn("int TileXRCcuDirectRuntime::QueryBasicInfo", source)
        self.assertIn("int TileXRCcuDirectRuntime::CreateDriverAdapter", source)
        self.assertIn("loader_.Load", source)
        self.assertIn("loader_.ResolveDevicePhyId", source)
        self.assertIn("raCustomChannelProvider_.Init(devicePhyId_, loader_.RaCustomChannel", source)
        self.assertIn("raCustomChannelProvider_.CreateAdapter(adapter, report)", source)
        self.assertIn("adapter.GetDieEnabled(dieId, &enabled", source)
        self.assertIn("adapter.GetBasicInfo(dieId, basicInfo", source)

        combined = header + "\n" + source
        for needle in [
            "udma/",
            "#include <hcomm/",
            "#include <hccl/",
            "libhcomm",
            "libhccl_v2",
            "libhccl_fwk",
            "libmc2_client",
            "HcclGetCcuTaskInfo",
            "HcomGetCcuTaskInfo",
            "CcuResBatchAllocator",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)

    def test_direct_ccu_runtime_keeps_ra_custom_channel_provider_alive_for_created_adapters(self):
        header = DIRECT_RUNTIME_HEADER.read_text(encoding="utf-8")
        source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")

        self.assertIn("tilexr_ccu_ra_custom_channel_provider.h", header)
        self.assertIn("TileXRCcuRaCustomChannelProvider raCustomChannelProvider_", header)
        self.assertIn("raCustomChannelProvider_.Init(devicePhyId_, loader_.RaCustomChannel", source)
        self.assertIn("raCustomChannelProvider_.CreateAdapter(adapter, report)", source)
        self.assertNotIn("TileXRCcuRaCustomChannelProvider provider;", source)

    def test_ccu_hccp_loader_keeps_private_hcomm_hccl_dependencies_out(self):
        combined = (
            ABI_HEADER.read_text(encoding="utf-8")
            + "\n"
            + TYPES_HEADER.read_text(encoding="utf-8")
            + "\n"
            + LOADER_HEADER.read_text(encoding="utf-8")
            + "\n"
            + LOADER_SOURCE.read_text(encoding="utf-8")
        )

        self.assertIn("libra.so", combined)
        self.assertIn("libruntime.so", combined)
        self.assertIn("TILEXR_CCU_EID_BYTES", ABI_HEADER.read_text(encoding="utf-8"))
        self.assertIn('tilexr_ccu_abi_constants.h', TYPES_HEADER.read_text(encoding="utf-8"))
        self.assertNotIn('tilexr_ccu_lower_layer_payloads.h', TYPES_HEADER.read_text(encoding="utf-8"))
        for needle in [
            "udma/",
            "#include <hcomm/",
            "#include <hccl/",
            "libhcomm",
            "libhccl_v2",
            "libhccl_fwk",
            "libmc2_client",
            "HcclGetCcuTaskInfo",
            "HcomGetCcuTaskInfo",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)

    def test_ccu_hccp_loader_loads_optional_tilexr_endpoint_route_provider(self):
        code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #include "tilexr_types.h"

            #include <cstdlib>
            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuHccpLoader loader;
                TileXRCcuHccpLoaderReport report;
                if (loader.LoadEndpointRouteProviderFromEnv(&report) != TILEXR_SUCCESS) {
                    std::cerr << "load provider failed: " << report.message << "\n";
                    return 1;
                }
                if (loader.CollectLocalEndpointRoute == nullptr) {
                    std::cerr << "endpoint route provider function missing\n";
                    return 2;
                }

                TileXRCcuEndpointRouteProviderResourceWindow window;
                window.addr = 0x10000000ULL;
                window.bytes = 0x2000;
                window.tokenId = 0x1234;
                window.rawTokenId = 0x2234;
                window.tokenValue = 0;

                TileXRCcuEndpointRouteProviderRoute route;
                if (loader.CollectLocalEndpointRoute(0x55, &window, &route) != TILEXR_SUCCESS) {
                    std::cerr << "provider call failed\n";
                    return 3;
                }
                if (!route.endpointRouteVerified || route.remoteEid[0] != 0xaa ||
                    route.tpn != 0x10203 || route.doorbellVa != 0x1122334455667788ULL ||
                    route.doorbellTokenId != 0x3456 || route.doorbellTokenValue != 0 ||
                    route.sqDepth != 64) {
                    std::cerr << "route mismatch\n";
                    return 4;
                }
                if (!report.endpointRouteProviderLoaded) {
                    std::cerr << "provider report did not record loaded state\n";
                    return 5;
                }
                return 0;
            }
            '''
        )
        plugin_code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #include "tilexr_types.h"

            #include <cstring>

            using namespace TileXR;

            extern "C" int TileXRCcuCollectLocalEndpointRoute(
                uint32_t devicePhyId,
                const TileXRCcuEndpointRouteProviderResourceWindow* window,
                TileXRCcuEndpointRouteProviderRoute* route)
            {
                if (devicePhyId != 0x55 || window == nullptr || route == nullptr ||
                    window->addr != 0x10000000ULL || window->tokenId != 0x1234) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                std::memset(route, 0, sizeof(*route));
                route->remoteEid[0] = 0xaa;
                route->tpn = 0x10203;
                route->doorbellVa = 0x1122334455667788ULL;
                route->doorbellTokenId = 0x3456;
                route->doorbellTokenValue = 0;
                route->sqDepth = 64;
                route->endpointRouteVerified = true;
                return TILEXR_SUCCESS;
            }
            '''
        )

        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            plugin_cpp = temp_path / "tilexr_ccu_endpoint_route_provider.cpp"
            plugin_so = temp_path / "tilexr_ccu_endpoint_route_provider.so"
            plugin_cpp.write_text(plugin_code, encoding="utf-8")
            plugin_build = subprocess.run(
                [
                    compiler,
                    "-std=c++14",
                    "-shared",
                    "-fPIC",
                    "-I",
                    str(REPO_ROOT / "src" / "include"),
                    "-I",
                    str(REPO_ROOT / "src" / "comm"),
                    str(plugin_cpp),
                    "-o",
                    str(plugin_so),
                ],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertEqual(0, plugin_build.returncode, plugin_build.stdout + plugin_build.stderr)
            result = self.compile_and_run(
                code,
                extra_sources=[LOADER_SOURCE],
                extra_link_flags=["-ldl"],
                extra_env={"TILEXR_CCU_ENDPOINT_ROUTE_PROVIDER": str(plugin_so)},
            )

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_runtime_uses_optional_tilexr_endpoint_route_provider_before_env(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            #include <cstring>
            #include <iostream>

            using namespace TileXR;

            int EchoLocalExchangeAsPeer(const void* sendBuf, size_t sendBytes, void* recvBuf, void*)
            {
                if (sendBuf == nullptr || sendBytes != sizeof(TileXRCcuResourceWindowExchange)) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                const auto* local = static_cast<const TileXRCcuResourceWindowExchange*>(sendBuf);
                auto* out = static_cast<TileXRCcuResourceWindowExchange*>(recvBuf);
                std::memset(out, 0, sizeof(TileXRCcuResourceWindowExchange) * 2);
                out[0] = *local;
                return TILEXR_SUCCESS;
            }

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.devicePhyId_ = 0x55;
                runtime.options_.rank = 1;
                runtime.options_.rankSize = 2;
                runtime.options_.allGather = EchoLocalExchangeAsPeer;
                runtime.localResourceWindow_.addr = 0x10000000ULL;
                runtime.localResourceWindow_.bytes = 0x2000;
                runtime.localResourceWindow_.tokenId = 0x1234;
                runtime.localResourceWindow_.rawTokenId = 0x2234;
                runtime.localResourceWindow_.tokenValue = 0;

                TileXRCcuDirectRuntimeReport report;
                if (runtime.RefreshLocalVerifiedEndpointRoute(&report) != TILEXR_SUCCESS) {
                    std::cerr << "provider route was not collected: " << report.message << "\n";
                    return 1;
                }

                std::vector<TileXRCcuRemoteCcuBufferInfo> buffers;
                if (runtime.ExportRemoteCcuRmaBuffers(&buffers) != TILEXR_SUCCESS) {
                    std::cerr << "remote export failed\n";
                    return 2;
                }
                if (buffers.size() != 1 || !buffers[0].endpointRouteVerified ||
                    buffers[0].remoteEid[0] != 0xaa ||
                    buffers[0].tpn != 0x10203 ||
                    buffers[0].doorbellVa != 0x1122334455667788ULL ||
                    buffers[0].doorbellTokenId != 0x3456 ||
                    buffers[0].doorbellTokenValue != 0 ||
                    buffers[0].sqDepth != 64) {
                    std::cerr << "provider route was not exported\n";
                    return 3;
                }
                return 0;
            }
            '''
        )
        plugin_code = textwrap.dedent(
            r'''
            #include "ccu/tilexr_ccu_hccp_loader.h"
            #include "tilexr_types.h"

            #include <cstring>

            using namespace TileXR;

            extern "C" int TileXRCcuCollectLocalEndpointRoute(
                uint32_t devicePhyId,
                const TileXRCcuEndpointRouteProviderResourceWindow* window,
                TileXRCcuEndpointRouteProviderRoute* route)
            {
                if (devicePhyId != 0x55 || window == nullptr || route == nullptr ||
                    window->addr != 0x10000000ULL || window->tokenId != 0x1234) {
                    return TILEXR_ERROR_PARA_CHECK_FAIL;
                }
                std::memset(route, 0, sizeof(*route));
                route->remoteEid[0] = 0xaa;
                route->tpn = 0x10203;
                route->doorbellVa = 0x1122334455667788ULL;
                route->doorbellTokenId = 0x3456;
                route->doorbellTokenValue = 0;
                route->sqDepth = 64;
                route->endpointRouteVerified = true;
                return TILEXR_SUCCESS;
            }
            '''
        )

        compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("no local C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            plugin_cpp = temp_path / "tilexr_ccu_endpoint_route_provider.cpp"
            plugin_so = temp_path / "tilexr_ccu_endpoint_route_provider.so"
            plugin_cpp.write_text(plugin_code, encoding="utf-8")
            plugin_build = subprocess.run(
                [
                    compiler,
                    "-std=c++14",
                    "-shared",
                    "-fPIC",
                    "-I",
                    str(REPO_ROOT / "src" / "include"),
                    "-I",
                    str(REPO_ROOT / "src" / "comm"),
                    str(plugin_cpp),
                    "-o",
                    str(plugin_so),
                ],
                cwd=REPO_ROOT,
                check=False,
                text=True,
                capture_output=True,
            )
            self.assertEqual(0, plugin_build.returncode, plugin_build.stdout + plugin_build.stderr)
            result = self.compile_and_run(
                code,
                extra_sources=[
                    DIRECT_RUNTIME_SOURCE,
                    LOADER_SOURCE,
                    REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                    REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
                    REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.cpp",
                ],
                extra_link_flags=["-ldl"],
                extra_env={"TILEXR_CCU_ENDPOINT_ROUTE_PROVIDER": str(plugin_so)},
            )

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_runtime_collects_ra_ctx_loop_endpoint_route_before_env_fallback(self):
        source = DIRECT_RUNTIME_SOURCE.read_text(encoding="utf-8")

        self.assertIn("TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_COLLECTION_MODE", source)
        self.assertIn("ra_ctx_loop", source)
        self.assertIn("CollectLocalEndpointRouteWithRaCtx(&route)", source)

        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            #include <cstring>
            #include <iostream>

            using namespace TileXR;

            constexpr uint32_t kRawDbTokenId = 0x345600U;
            constexpr uint32_t kTokenValue = 0x7799U;
            constexpr uint64_t kTpHandle = 0xabcddcbaULL;
            constexpr uint64_t kCcuResourceBase = 0x10000000ULL;
            constexpr uint64_t kHcommWqeBasicBlockOffset = 0x1000000ULL;
            constexpr uint64_t kHcommSqBufferSize = 256ULL * 1024ULL;
            constexpr uint32_t kDirectLoopJettyCtxId = 0;
            constexpr uint64_t kExpectedSqVa =
                kCcuResourceBase + kHcommWqeBasicBlockOffset +
                static_cast<uint64_t>(kDirectLoopJettyCtxId) * kHcommSqBufferSize;

            int g_cqCreateCalls = 0;
            int g_qpCreateCalls = 0;
            int g_tpInfoCalls = 0;
            int g_reqResultCalls = 0;
            int g_qpImportCalls = 0;
            int g_qpBindCalls = 0;
            int g_qpUnbindCalls = 0;
            int g_qpUnimportCalls = 0;
            int g_qpDestroyCalls = 0;
            int g_cqDestroyCalls = 0;
            TileXRCcuHccpQpCreateAttr g_lastQpAttr {};
            TileXRCcuHccpGetTpCfg g_lastTpCfg {};
            TileXRCcuHccpQpImportInfo g_lastImportInfo {};

            int FakeCqCreate(void* ctx, TileXRCcuHccpCqInfo* info, void** cqHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000) || info == nullptr || cqHandle == nullptr) {
                    return -1;
                }
                if (info->in.depth != 64 || info->in.ub.mode != 2) {
                    return -2;
                }
                ++g_cqCreateCalls;
                *cqHandle = reinterpret_cast<void*>(0x2000);
                return 0;
            }

            int FakeCqDestroy(void*, void* cqHandle)
            {
                if (cqHandle == reinterpret_cast<void*>(0x2000)) {
                    ++g_cqDestroyCalls;
                }
                return 0;
            }

            int FakeQpCreate(
                void* ctx,
                TileXRCcuHccpQpCreateAttr* attr,
                TileXRCcuHccpQpCreateInfo* info,
                void** qpHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000) || attr == nullptr || info == nullptr ||
                    qpHandle == nullptr) {
                    return -3;
                }
                g_lastQpAttr = *attr;
                if (attr->scqHandle != reinterpret_cast<void*>(0x2000) ||
                    attr->rcqHandle != reinterpret_cast<void*>(0x2000) ||
                    attr->sqDepth != 8 ||
                    attr->rqDepth != TILEXR_CCU_HCCP_RQ_DEPTH_DEFAULT ||
                    attr->transportMode != TILEXR_CCU_HCCP_TRANSPORT_MODE_RM ||
                    attr->ub.mode != static_cast<int>(TILEXR_CCU_HCCP_JETTY_MODE_CCU) ||
                    attr->ub.jettyId != 1024 ||
                    attr->ub.tokenIdHandle != reinterpret_cast<void*>(0x3000) ||
                    attr->ub.tokenValue != kTokenValue ||
                    attr->ub.flag.bs.shareJfr != 1 ||
                    attr->ub.jfsFlag.bs.errorSuspend != 1 ||
                    attr->ub.extMode.cstmFlag.bs.sqCstm != 1 ||
                    attr->ub.extMode.sqebbNum != 8 ||
                    attr->ub.extMode.sq.buffVa != kExpectedSqVa ||
                    attr->ub.extMode.sq.buffSize != 8 * 4 * 64) {
                    return -4;
                }
                for (uint32_t i = 0; i < TILEXR_CCU_HCCP_QP_KEY_BYTES; ++i) {
                    info->key.value[i] = static_cast<uint8_t>(0x80U + i);
                }
                info->key.size = TILEXR_CCU_HCCP_QP_KEY_BYTES;
                info->ub.dbAddr = 0x1122334455667788ULL;
                info->ub.dbTokenId = kRawDbTokenId;
                ++g_qpCreateCalls;
                *qpHandle = reinterpret_cast<void*>(0x4000);
                return 0;
            }

            int FakeQpDestroy(void* qpHandle)
            {
                if (qpHandle == reinterpret_cast<void*>(0x4000)) {
                    ++g_qpDestroyCalls;
                }
                return 0;
            }

            int FakeGetTpInfoListAsync(
                void* ctx,
                TileXRCcuHccpGetTpCfg* cfg,
                TileXRCcuHccpTpInfo infoList[],
                uint32_t* num,
                void** reqHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000) || cfg == nullptr || infoList == nullptr ||
                    num == nullptr || reqHandle == nullptr || *num != 1) {
                    return -5;
                }
                g_lastTpCfg = *cfg;
                if (cfg->flag.bs.rtp != 1 || cfg->transMode != TILEXR_CCU_HCCP_TRANSPORT_MODE_RM ||
                    std::memcmp(cfg->localEid.raw, cfg->peerEid.raw, TILEXR_CCU_EID_BYTES) != 0) {
                    return -6;
                }
                infoList[0].tpHandle = kTpHandle;
                ++g_tpInfoCalls;
                *reqHandle = reinterpret_cast<void*>(0x5000);
                return 0;
            }

            int FakeGetAsyncReqResult(void* reqHandle, int* reqResult)
            {
                if (reqHandle != reinterpret_cast<void*>(0x5000) || reqResult == nullptr) {
                    return -7;
                }
                *reqResult = 0;
                ++g_reqResultCalls;
                return 0;
            }

            int FakeQpImport(void* ctx, TileXRCcuHccpQpImportInfo* info, void** remoteQpHandle)
            {
                if (ctx != reinterpret_cast<void*>(0x1000) || info == nullptr || remoteQpHandle == nullptr) {
                    return -8;
                }
                g_lastImportInfo = *info;
                if (info->in.key.size != TILEXR_CCU_HCCP_QP_KEY_BYTES ||
                    info->in.key.value[0] != 0x80 ||
                    info->in.ub.mode != TILEXR_CCU_HCCP_JETTY_IMPORT_MODE_EXP ||
                    info->in.ub.tokenValue != kTokenValue ||
                    info->in.ub.policy != TILEXR_CCU_HCCP_JETTY_GRP_POLICY_RR ||
                    info->in.ub.type != TILEXR_CCU_HCCP_TARGET_TYPE_JETTY ||
                    info->in.ub.flag.bs.tokenPolicy != TILEXR_CCU_HCCP_TOKEN_POLICY_PLAIN_TEXT ||
                    info->in.ub.expImportCfg.tpHandle != kTpHandle ||
                    info->in.ub.expImportCfg.peerTpHandle != kTpHandle ||
                    info->in.ub.expImportCfg.txPsn == 0 ||
                    info->in.ub.expImportCfg.rxPsn == 0 ||
                    info->in.ub.tpType != TILEXR_CCU_HCCP_TP_TYPE_RTP) {
                    return -9;
                }
                info->out.ub.tpn = 0;
                ++g_qpImportCalls;
                *remoteQpHandle = reinterpret_cast<void*>(0x6000);
                return 0;
            }

            int FakeQpUnimport(void*, void* remoteQpHandle)
            {
                if (remoteQpHandle == reinterpret_cast<void*>(0x6000)) {
                    ++g_qpUnimportCalls;
                }
                return 0;
            }

            int FakeQpBind(void* qpHandle, void* remoteQpHandle)
            {
                if (qpHandle != reinterpret_cast<void*>(0x4000) ||
                    remoteQpHandle != reinterpret_cast<void*>(0x6000)) {
                    return -10;
                }
                ++g_qpBindCalls;
                return -11;
            }

            int FakeQpUnbind(void* qpHandle)
            {
                if (qpHandle == reinterpret_cast<void*>(0x4000)) {
                    ++g_qpUnbindCalls;
                }
                return 0;
            }

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.devicePhyId_ = 0x55;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 1;
                runtime.localResourceWindow_.addr = kCcuResourceBase;
                runtime.localResourceWindow_.bytes = TILEXR_CCU_RESOURCE_WINDOW_BYTES;
                runtime.localResourceWindow_.tokenId = 0x3456;
                runtime.localResourceWindow_.rawTokenId = 0x345600;
                runtime.localResourceWindow_.tokenValue = kTokenValue;
                runtime.localResourceWindow_.raCtxHandle = reinterpret_cast<void*>(0x1000);
                runtime.localResourceWindow_.tokenIdHandle = reinterpret_cast<void*>(0x3000);
                runtime.localResourceWindow_.raCtxRegistered = true;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    runtime.localResourceWindow_.eid[i] = static_cast<uint8_t>(0xa0U + i);
                }

                runtime.loader_.RaCtxCqCreate = FakeCqCreate;
                runtime.loader_.RaCtxCqDestroy = FakeCqDestroy;
                runtime.loader_.RaCtxQpCreate = FakeQpCreate;
                runtime.loader_.RaCtxQpDestroy = FakeQpDestroy;
                runtime.loader_.RaGetTpInfoListAsync = FakeGetTpInfoListAsync;
                runtime.loader_.RaGetAsyncReqResult = FakeGetAsyncReqResult;
                runtime.loader_.RaCtxQpImport = FakeQpImport;
                runtime.loader_.RaCtxQpUnimport = FakeQpUnimport;
                runtime.loader_.RaCtxQpBind = FakeQpBind;
                runtime.loader_.RaCtxQpUnbind = FakeQpUnbind;

                TileXRCcuDirectRuntimeReport report;
                if (runtime.RefreshLocalVerifiedEndpointRoute(&report) != TILEXR_SUCCESS) {
                    std::cerr << "ra ctx loop route collection failed: " << report.message << "\n";
                    return 1;
                }
                if (!runtime.localVerifiedEndpointRouteValid_) {
                    std::cerr << "verified route flag not set\n";
                    return 2;
                }
                const auto& route = runtime.localVerifiedEndpointRoute_;
                if (!route.endpointRouteVerified ||
                    route.remoteEid[0] != 0xa0 ||
                    route.tpn != 0 ||
                    route.doorbellVa != 0x1122334455667788ULL ||
                    route.doorbellTokenId != (kRawDbTokenId >> 8) ||
                    route.doorbellTokenValue != kTokenValue ||
                    route.sqDepth != 8) {
                    std::cerr << "verified route mismatch\n";
                    return 3;
                }
                if (g_cqCreateCalls != 1 || g_qpCreateCalls != 1 || g_tpInfoCalls != 1 ||
                    g_reqResultCalls != 1 || g_qpImportCalls != 1 || g_qpBindCalls != 0) {
                    std::cerr << "unexpected RA call counts\n";
                    return 4;
                }

                runtime.Shutdown();
                if (g_qpUnbindCalls != 0 || g_qpUnimportCalls != 1 ||
                    g_qpDestroyCalls != 1 || g_cqDestroyCalls != 1) {
                    std::cerr << "endpoint route handles were not released\n";
                    return 5;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                LOADER_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.cpp",
            ],
            extra_link_flags=["-ldl"],
            extra_env={
                "TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_COLLECTION_MODE": "ra_ctx_loop",
            },
        )

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_runtime_retries_ra_ctx_endpoint_route_after_async_result_failure(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            #include <cstring>
            #include <iostream>

            using namespace TileXR;

            constexpr uint32_t kRawDbTokenId = 0x456700U;
            constexpr uint32_t kTokenValue = 0x8811U;
            constexpr uint64_t kCcuResourceBase = 0x11000000ULL;

            int g_cqCreateCalls = 0;
            int g_qpCreateCalls = 0;
            int g_tpInfoCalls = 0;
            int g_reqResultCalls = 0;
            int g_qpImportCalls = 0;
            int g_qpDestroyCalls = 0;
            int g_cqDestroyCalls = 0;

            int FakeCqCreate(void*, TileXRCcuHccpCqInfo*, void** cqHandle)
            {
                if (cqHandle == nullptr) {
                    return -1;
                }
                ++g_cqCreateCalls;
                *cqHandle = reinterpret_cast<void*>(0x2000 + g_cqCreateCalls);
                return 0;
            }

            int FakeCqDestroy(void*, void* cqHandle)
            {
                if (cqHandle != nullptr) {
                    ++g_cqDestroyCalls;
                }
                return 0;
            }

            int FakeQpCreate(
                void*,
                TileXRCcuHccpQpCreateAttr*,
                TileXRCcuHccpQpCreateInfo* info,
                void** qpHandle)
            {
                if (info == nullptr || qpHandle == nullptr) {
                    return -2;
                }
                ++g_qpCreateCalls;
                for (uint32_t i = 0; i < TILEXR_CCU_HCCP_QP_KEY_BYTES; ++i) {
                    info->key.value[i] = static_cast<uint8_t>(0x40U + i);
                }
                info->key.size = TILEXR_CCU_HCCP_QP_KEY_BYTES;
                info->ub.dbAddr = 0x1222333444555666ULL;
                info->ub.dbTokenId = kRawDbTokenId;
                *qpHandle = reinterpret_cast<void*>(0x4000 + g_qpCreateCalls);
                return 0;
            }

            int FakeQpDestroy(void* qpHandle)
            {
                if (qpHandle != nullptr) {
                    ++g_qpDestroyCalls;
                }
                return 0;
            }

            int FakeGetTpInfoListAsync(
                void*,
                TileXRCcuHccpGetTpCfg*,
                TileXRCcuHccpTpInfo infoList[],
                uint32_t* num,
                void** reqHandle)
            {
                if (infoList == nullptr || num == nullptr || reqHandle == nullptr || *num != 1) {
                    return -3;
                }
                ++g_tpInfoCalls;
                infoList[0].tpHandle = 0xabcd0000ULL + g_tpInfoCalls;
                *reqHandle = reinterpret_cast<void*>(0x5000 + g_tpInfoCalls);
                return 0;
            }

            int FakeGetAsyncReqResult(void*, int* reqResult)
            {
                if (reqResult == nullptr) {
                    return -4;
                }
                ++g_reqResultCalls;
                *reqResult = (g_reqResultCalls <= 3) ? 528101 : 0;
                return 0;
            }

            int FakeQpImport(void*, TileXRCcuHccpQpImportInfo* info, void** remoteQpHandle)
            {
                if (info == nullptr || remoteQpHandle == nullptr) {
                    return -5;
                }
                ++g_qpImportCalls;
                info->out.ub.tpn = 0x7000 + g_qpImportCalls;
                *remoteQpHandle = reinterpret_cast<void*>(0x6000 + g_qpImportCalls);
                return 0;
            }

            int FakeQpUnimport(void*, void*)
            {
                return 0;
            }

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.devicePhyId_ = 0x55;
                runtime.options_.rank = 0;
                runtime.options_.rankSize = 1;
                runtime.localResourceWindow_.addr = kCcuResourceBase;
                runtime.localResourceWindow_.bytes = TILEXR_CCU_RESOURCE_WINDOW_BYTES;
                runtime.localResourceWindow_.tokenId = 0x4567;
                runtime.localResourceWindow_.rawTokenId = kRawDbTokenId;
                runtime.localResourceWindow_.tokenValue = kTokenValue;
                runtime.localResourceWindow_.raCtxHandle = reinterpret_cast<void*>(0x1000);
                runtime.localResourceWindow_.tokenIdHandle = reinterpret_cast<void*>(0x3000);
                runtime.localResourceWindow_.raCtxRegistered = true;
                for (uint32_t i = 0; i < TILEXR_CCU_EID_BYTES; ++i) {
                    runtime.localResourceWindow_.eid[i] = static_cast<uint8_t>(0xb0U + i);
                }

                runtime.loader_.RaCtxCqCreate = FakeCqCreate;
                runtime.loader_.RaCtxCqDestroy = FakeCqDestroy;
                runtime.loader_.RaCtxQpCreate = FakeQpCreate;
                runtime.loader_.RaCtxQpDestroy = FakeQpDestroy;
                runtime.loader_.RaGetTpInfoListAsync = FakeGetTpInfoListAsync;
                runtime.loader_.RaGetAsyncReqResult = FakeGetAsyncReqResult;
                runtime.loader_.RaCtxQpImport = FakeQpImport;
                runtime.loader_.RaCtxQpUnimport = FakeQpUnimport;

                TileXRCcuDirectRuntimeReport report;
                const int ret = runtime.RefreshLocalVerifiedEndpointRoute(&report);
                if (ret != TILEXR_SUCCESS) {
                    std::cerr << "ra ctx endpoint route retry failed: " << report.message
                              << " ret=" << ret << "\n";
                    return 1;
                }
                if (!runtime.localVerifiedEndpointRouteValid_) {
                    std::cerr << "verified endpoint route flag not set after retry\n";
                    return 2;
                }
                const auto& route = runtime.localVerifiedEndpointRoute_;
                if (!route.endpointRouteVerified ||
                    route.remoteEid[0] != 0xb0 ||
                    route.doorbellVa != 0x1222333444555666ULL ||
                    route.doorbellTokenId != (kRawDbTokenId >> 8) ||
                    route.doorbellTokenValue != kTokenValue ||
                    route.sqDepth != 8) {
                    std::cerr << "verified endpoint route mismatch after retry\n";
                    return 3;
                }
                if (g_cqCreateCalls != 4 || g_qpCreateCalls != 4 || g_tpInfoCalls != 4 ||
                    g_reqResultCalls != 4 || g_qpImportCalls != 1) {
                    std::cerr << "unexpected retry call counts"
                              << " cqCreate=" << g_cqCreateCalls
                              << " qpCreate=" << g_qpCreateCalls
                              << " tpInfo=" << g_tpInfoCalls
                              << " reqResult=" << g_reqResultCalls
                              << " qpImport=" << g_qpImportCalls << "\n";
                    return 4;
                }
                if (g_qpDestroyCalls != 3 || g_cqDestroyCalls != 3) {
                    std::cerr << "failed attempt was not cleaned up before retry"
                              << " qpDestroy=" << g_qpDestroyCalls
                              << " cqDestroy=" << g_cqDestroyCalls << "\n";
                    return 5;
                }

                runtime.Shutdown();
                if (g_qpDestroyCalls != 4 || g_cqDestroyCalls != 4) {
                    std::cerr << "successful retry handles were not released on shutdown"
                              << " qpDestroy=" << g_qpDestroyCalls
                              << " cqDestroy=" << g_cqDestroyCalls << "\n";
                    return 6;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                LOADER_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.cpp",
            ],
            extra_link_flags=["-ldl"],
            extra_env={
                "TILEXR_CCU_DIRECT_ENDPOINT_ROUTE_COLLECTION_MODE": "ra_ctx_loop",
            },
        )

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_direct_runtime_fails_closed_when_configured_endpoint_route_provider_cannot_load(self):
        code = textwrap.dedent(
            r'''
            #define private public
            #include "ccu/tilexr_ccu_direct_runtime.h"
            #undef private

            #include <iostream>

            using namespace TileXR;

            int main()
            {
                TileXRCcuDirectRuntime runtime;
                runtime.initialized_ = true;
                runtime.resourceWindowRegistered_ = true;
                runtime.devicePhyId_ = 0x55;
                runtime.options_.rank = 1;
                runtime.options_.rankSize = 2;
                runtime.localResourceWindow_.addr = 0x10000000ULL;
                runtime.localResourceWindow_.bytes = 0x2000;
                runtime.localResourceWindow_.tokenId = 0x1234;
                runtime.localResourceWindow_.rawTokenId = 0x2234;
                runtime.localResourceWindow_.tokenValue = 0;

                TileXRCcuDirectRuntimeReport report;
                const int ret = runtime.RefreshLocalVerifiedEndpointRoute(&report);
                if (ret == TILEXR_SUCCESS) {
                    std::cerr << "configured bad provider fell back to env route\n";
                    return 1;
                }
                if (runtime.localVerifiedEndpointRouteValid_) {
                    std::cerr << "bad provider left a verified route\n";
                    return 2;
                }
                if (report.message.find("failed to load direct CCU endpoint route provider") == std::string::npos) {
                    std::cerr << "unexpected failure message: " << report.message << "\n";
                    return 3;
                }
                return 0;
            }
            '''
        )
        result = self.compile_and_run(
            code,
            extra_sources=[
                DIRECT_RUNTIME_SOURCE,
                LOADER_SOURCE,
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_ra_custom_channel_provider.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_driver_adapter.cpp",
                REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_specs.cpp",
            ],
            extra_link_flags=["-ldl"],
            extra_env={
                "TILEXR_CCU_ENDPOINT_ROUTE_PROVIDER": "/tmp/tilexr_missing_endpoint_route_provider.so",
                "TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_EID_RANK1": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_TPN_RANK1": "0x10203",
                "TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_VA_RANK1": "0x1122334455667788",
                "TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_TOKEN_ID_RANK1": "0x3456",
                "TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_DOORBELL_TOKEN_VALUE_RANK1": "0",
                "TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_SQ_DEPTH_RANK1": "64",
            },
        )

        self.assertEqual("", result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
