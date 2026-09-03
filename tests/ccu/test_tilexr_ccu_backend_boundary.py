#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
COMM_HEADER = REPO_ROOT / "src" / "comm" / "tilexr_comm.h"
PUBLIC_API_HEADER = REPO_ROOT / "src" / "include" / "tilexr_api.h"
BACKEND_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_backend.h"
BACKEND_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_backend.cpp"
RUNTIME_SESSION_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_runtime_session.h"
RUNTIME_SESSION_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_runtime_session.cpp"
PLANNER_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_collective_planner.h"
PLANNER_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_collective_planner.cpp"
EXECUTOR_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_executor.h"
EXECUTOR_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_executor.cpp"


class TileXRCcuBackendBoundaryTest(unittest.TestCase):
    def test_backend_files_exist(self):
        self.assertTrue(BACKEND_HEADER.exists())
        self.assertTrue(BACKEND_SOURCE.exists())

    def test_backend_internals_are_split(self):
        for path in [
            RUNTIME_SESSION_HEADER,
            RUNTIME_SESSION_SOURCE,
            PLANNER_HEADER,
            PLANNER_SOURCE,
            EXECUTOR_HEADER,
            EXECUTOR_SOURCE,
        ]:
            with self.subTest(path=path.name):
                self.assertTrue(path.exists())

    def test_tilexr_comm_header_owns_only_opaque_backend(self):
        header = COMM_HEADER.read_text(encoding="utf-8")
        self.assertIn("class TileXRCcuBackend;", header)
        self.assertIn("std::unique_ptr<TileXRCcuBackend> ccuBackend_", header)
        for needle in [
            "tilexr_ccu_direct_orchestrator.h",
            "tilexr_ccu_direct_runtime.h",
            "tilexr_ccu_lower_layer_plan_builder.h",
            "TileXRCcuDirectRuntime",
            "directCcuBasicInfo_",
            "directCcuLowerLayerPlan_",
            "directCcuVerifiedEndpointRoutes_",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, header)

    def test_backend_header_exposes_facade_not_public_c_api(self):
        header = BACKEND_HEADER.read_text(encoding="utf-8")
        public_api = PUBLIC_API_HEADER.read_text(encoding="utf-8")
        self.assertIn("class TileXRCcuBackend", header)
        self.assertIn("struct TileXRCcuBackendOptions", header)
        self.assertIn("TileXRSockExchange *exchange", header)
        self.assertIn("PrepareCollective", header)
        self.assertIn("SubmitCollective", header)
        for needle in [
            "enum class TileXRCcuSignalWaitRole",
            "struct TileXRCcuSignalWaitRequest",
            "struct TileXRCcuSignalWaitPlan",
            "PrepareSignalWait",
            "SubmitSignalWait",
        ]:
            with self.subTest(internal=needle):
                self.assertIn(needle, header)
        for needle in [
            "TileXRDirectCcuPreparedTasksPtr",
            "TileXRCommPrepareDirectCcu",
            "TileXRDirectCcuSubmitPrepared",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, header)
                self.assertNotIn(needle, public_api)
        for needle in [
            "TileXRCcuSignalWait",
            "PrepareSignalWait",
            "SubmitSignalWait",
        ]:
            with self.subTest(public_needle=needle):
                self.assertNotIn(needle, public_api)

    def test_split_sources_own_restored_direct_ccu_runtime_glue(self):
        source = BACKEND_SOURCE.read_text(encoding="utf-8")
        runtime_header = RUNTIME_SESSION_HEADER.read_text(encoding="utf-8")
        runtime = RUNTIME_SESSION_SOURCE.read_text(encoding="utf-8")
        planner = PLANNER_SOURCE.read_text(encoding="utf-8")
        executor = EXECUTOR_SOURCE.read_text(encoding="utf-8")
        self.assertIn("#include \"ccu/tilexr_ccu_runtime_session.h\"", source)
        self.assertIn("#include \"ccu/tilexr_ccu_collective_planner.h\"", source)
        self.assertIn("#include \"ccu/tilexr_ccu_executor.h\"", source)
        self.assertNotIn("#include \"ccu/tilexr_ccu_direct_runtime.h\"", source)
        self.assertNotIn("#include \"ccu/tilexr_ccu_repository.h\"", source)
        self.assertNotIn("TileXRCcuDirectRuntime", source)

        for needle in [
            "#include \"ccu/tilexr_ccu_direct_runtime.h\"",
            "TileXRCcuDirectRuntime",
            "DirectCcuThreadAllGather",
            "g_directCcuAllGatherStates",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, runtime_header + "\n" + runtime)

        for needle in [
            "#include \"ccu/tilexr_ccu_repository.h\"",
            "PrepareDirectCcuInstallAttempt",
            "PrepareSignalWait",
            "PrepareDirectCcuLowerLayerPlanCallback",
            "TileXRCcuRunDirectInstallAttempt",
            "TileXRCcuRunDirectSignalWaitInstallAttempt",
            "TileXRCcuMakeRepositoryDeviceMemoryOps",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, planner)

        self.assertIn("planner_->PrepareSignalWait", source)
        self.assertNotIn("return TILEXR_ERROR_NOT_SUPPORT;", source[source.index("PrepareSignalWait"):])

        planner_header = PLANNER_HEADER.read_text(encoding="utf-8")
        executor_header = EXECUTOR_HEADER.read_text(encoding="utf-8")
        self.assertRegex(
            planner_header,
            r"(?s)#ifdef TILEXR_CCU_TESTING.*PrepareDirectCcuMemoryCopyInstallAttempt.*#endif",
        )
        self.assertRegex(
            planner,
            r"(?s)#ifdef TILEXR_CCU_TESTING.*PrepareDirectCcuMemoryCopyInstallAttempt.*#endif",
        )
        self.assertRegex(
            executor_header,
            r"(?s)#ifdef TILEXR_CCU_TESTING.*ReadDirectCcuInstructionsForDebug.*#endif",
        )
        self.assertRegex(
            executor,
            r"(?s)#ifdef TILEXR_CCU_TESTING.*ReadDirectCcuInstructionsForDebug.*#endif",
        )
        for fake_ready in [
            "options_ = options;\n    initialized_ = true;\n    return TILEXR_SUCCESS;",
            "plan->ready = true;\n    return TILEXR_SUCCESS;",
            "return plan.ready ? TILEXR_SUCCESS",
        ]:
            with self.subTest(fake_ready=fake_ready):
                self.assertNotIn(fake_ready, source + "\n" + runtime + "\n" + planner + "\n" + executor)

    def test_tilexr_comm_can_auto_initialize_ccu_backend_without_blocking_comm_init(self):
        source = (REPO_ROOT / "src" / "comm" / "tilexr_comm.cpp").read_text(encoding="utf-8")

        self.assertIn('constexpr const char* TILEXR_ENABLE_CCU_BACKEND_ENV = "TILEXR_ENABLE_CCU_BACKEND"', source)
        self.assertIn("bool ShouldEnableCcuBackend()", source)
        self.assertIn("int TileXRComm::InitCcuBackendIfEnabled()", source)
        self.assertIn("const int ccuRet = InitCcuBackend()", source)
        self.assertIn("TileXR CCU backend init failed, direct CCU disabled", source)
        self.assertIn("TileXR CCU backend initialized", source)

        process_init = source[source.index("int TileXRComm::Init()"): source.index("int TileXRComm::InitThread")]
        thread_init = source[source.index("int TileXRComm::InitThread"): source.index("int TileXRComm::EnablePeerAccess")]

        for body_name, body in [("process", process_init), ("thread", thread_init)]:
            with self.subTest(body=body_name):
                self.assertIn("ret = InitCcuBackendIfEnabled();", body)
                self.assertLess(body.index("ret = InitCcuBackendIfEnabled();"), body.index("ret = SyncCommArgs();"))
                self.assertIn("if (ret != TILEXR_SUCCESS) {", body)
                self.assertIn("return ret;", body)

        helper = source[
            source.index("int TileXRComm::InitCcuBackendIfEnabled()"):
            source.index("TileXRCcuBackend *TileXRComm::GetCcuBackendForCollectives")
        ]
        self.assertIn("if (!ShouldEnableCcuBackend())", helper)
        self.assertIn("return TILEXR_SUCCESS;", helper)
        self.assertNotIn("return ccuRet;", helper)

    def test_p2p_ccu_copy_process_token_is_not_urma_shifted(self):
        planner = PLANNER_SOURCE.read_text(encoding="utf-8")
        token_query = planner[
            planner.index("int QueryDirectCcuProcessMemoryToken"):
            planner.index("int BuildDirectCcuLocalMemoryCopyEndpoint")
        ]

        self.assertIn("rtUbDevQueryInfo(QUERY_PROCESS_TOKEN, &info)", token_query)
        self.assertIn("const uint32_t tokenId = info.tokenId;", token_query)
        self.assertIn("TileXRCcuPackMemoryToken(tokenId, info.tokenValue, true)", token_query)
        self.assertNotIn("info.tokenId >>", token_query)

    def test_p2p_ccu_copy_uses_original_va_for_microcode_and_imported_segva_for_route(self):
        planner = PLANNER_SOURCE.read_text(encoding="utf-8")
        endpoint_builder = planner[
            planner.index("int BuildDirectCcuLocalMemoryCopyEndpoint"):
            planner.index("void TileXRCcuCollectivePlanner::Reset")
        ]
        prepare_copy = planner[
            planner.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuMemoryCopyInstallAttempt"):
            planner.index("int TileXRCcuCollectivePlanner::RefreshDirectCcuLowerLayerPlan")
        ]

        self.assertIn("session.RegisterCcuResourceRmaBuffer(basicInfo->resourceAddr)", prepare_copy)
        self.assertIn("session.RegisterMemoryBuffer(sourceAddr, bytes, &sourceInfo)", endpoint_builder)
        self.assertIn("session.RegisterMemoryBuffer(destinationAddr, bytes, &destinationInfo)", endpoint_builder)
        self.assertIn("endpoint->sourceAddr = sourceInfo.addr", endpoint_builder)
        self.assertIn("endpoint->destinationAddr = destinationInfo.addr", endpoint_builder)
        self.assertNotIn("endpoint->sourceAddr = sourceInfo.targetSegVa", endpoint_builder)
        self.assertNotIn("endpoint->destinationAddr = destinationInfo.targetSegVa", endpoint_builder)
        self.assertIn("endpoint->sourceRemoteImport.key = sourceInfo.key", endpoint_builder)
        self.assertIn("endpoint->destinationRemoteImport.key = destinationInfo.key", endpoint_builder)
        self.assertIn("TileXRCcuPackMemoryToken(sourceInfo.tokenId, sourceInfo.tokenValue, true)", endpoint_builder)
        self.assertIn(
            "TileXRCcuPackMemoryToken(destinationInfo.tokenId, destinationInfo.tokenValue, true)",
            endpoint_builder,
        )
        self.assertIn("remoteImportRequest = peerEndpoint.sourceRemoteImport", prepare_copy)
        self.assertIn("remoteImportRequest = peerEndpoint.destinationRemoteImport", prepare_copy)
        self.assertIn("session.ImportRemoteMemoryBuffer(remoteImportRequest, &importedRemoteBuffer)", prepare_copy)
        self.assertIn("memoryCopy.remoteAddr = remoteImportRequest.addr", prepare_copy)
        self.assertIn("SetDirectCcuRemoteRouteMemoryOverride(", prepare_copy)
        self.assertIn("importedRemoteBuffer.targetSegVa", prepare_copy)
        self.assertNotIn("QueryDirectCcuProcessMemoryToken(sourceAddr", endpoint_builder)
        self.assertNotIn("QueryDirectCcuProcessMemoryToken(destinationAddr", endpoint_builder)

    def test_alltoall_overrides_only_copy_route_memory_not_sync_routes(self):
        planner = PLANNER_SOURCE.read_text(encoding="utf-8")
        prepare_alltoall = planner[
            planner.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuAllToAll2RankInstallAttempt"):
            planner.index("#endif", planner.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuAllToAll2RankInstallAttempt"))
        ]
        override_apply = planner[
            planner.index("void TileXRCcuCollectivePlanner::ApplyDirectCcuRemoteRouteMemoryOverride"):
            planner.index("#endif", planner.index("void TileXRCcuCollectivePlanner::ApplyDirectCcuRemoteRouteMemoryOverride"))
        ]

        self.assertIn("SetDirectCcuRemoteRouteMemoryOverrideForSyncRoute(", prepare_alltoall)
        self.assertIn("peerRanks.size() < routedPeerCount", planner)
        self.assertIn("0U", prepare_alltoall)
        self.assertIn("uint32_t routeIndex = 0", override_apply)
        self.assertIn("override.syncRouteIndex != routeIndex", override_apply)
        self.assertIn("++routeIndex", override_apply)
        self.assertIn("override.allRoutes", override_apply)

    def test_four_rank_mesh_gathers_imports_and_maps_three_routes_per_peer(self):
        header = PLANNER_HEADER.read_text(encoding="utf-8")
        planner = PLANNER_SOURCE.read_text(encoding="utf-8")

        self.assertIn("PrepareDirectCcuAllToAllMeshInstallAttempt", header)
        self.assertIn("PrepareDirectCcuAllToAllMeshInstallAttempt", planner)
        mesh_body = planner[
            planner.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuAllToAllMeshInstallAttempt"):
            planner.index("int TileXRCcuCollectivePlanner::PrepareDirectCcuSyncXnPingInstallAttempt")
        ]
        self.assertIn("rankSize < 2", mesh_body)
        self.assertIn("rankSize > 64", mesh_body)
        self.assertIn("rankSize - 1", mesh_body)
        self.assertEqual(1, mesh_body.count("session.AllGather("))
        self.assertIn("endpoint.rank != peerRank", mesh_body)
        self.assertIn("session.ImportRemoteMemoryBuffer", mesh_body)
        self.assertNotIn("routeWithinPeer", mesh_body)
        self.assertNotIn("SetDirectCcuRemoteRouteMemoryOverrideForSyncRoute", mesh_body)
        self.assertNotIn("peer.imported.targetSegVa", mesh_body)
        self.assertIn("ClearDirectCcuRemoteRouteMemoryOverride", mesh_body)
        self.assertIn("TileXRCcuRunDirectAllToAllMeshInstallAttempt", mesh_body)

        exchange = planner[
            planner.index("int TileXRCcuCollectivePlanner::ExchangeDirectCcuRemoteNotifyCke"):
            planner.index("void TileXRCcuCollectivePlanner::SetDirectCcuRemoteRouteMemoryOverride")
        ]
        self.assertIn("routesPerPeer = syncRouteCount / routedPeerCount", exchange)
        self.assertIn("peerBufferIndex = syncIndex / routesPerPeer", exchange)
        self.assertIn("peerLocalResourceOffset =", exchange)
        self.assertIn("peerLocalIndex * routesPerPeer + routeWithinPeer", exchange)

        self.assertIn("std::vector<DirectCcuRemoteRouteMemoryOverride>", header)
        self.assertIn("directCcuRemoteRouteMemoryOverrides_", header)
        override_apply = planner[
            planner.index("void TileXRCcuCollectivePlanner::ApplyDirectCcuRemoteRouteMemoryOverride"):
            planner.index("#endif", planner.index("void TileXRCcuCollectivePlanner::ApplyDirectCcuRemoteRouteMemoryOverride"))
        ]
        self.assertIn("for (const auto &override : directCcuRemoteRouteMemoryOverrides_)", override_apply)
        self.assertIn("override.syncRouteIndex != routeIndex", override_apply)


if __name__ == "__main__":
    unittest.main()
