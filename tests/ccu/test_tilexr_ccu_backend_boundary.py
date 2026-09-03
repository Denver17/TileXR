#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
COMM_HEADER = REPO_ROOT / "src" / "comm" / "tilexr_comm.h"
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
        self.assertIn("class TileXRCcuBackend", header)
        self.assertIn("struct TileXRCcuBackendOptions", header)
        self.assertIn("TileXRSockExchange *exchange", header)
        self.assertIn("PrepareCollective", header)
        self.assertIn("SubmitCollective", header)
        for needle in [
            "TileXRDirectCcuPreparedTasksPtr",
            "TileXRCommPrepareDirectCcu",
            "TileXRDirectCcuSubmitPrepared",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, header)

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
            "PrepareDirectCcuLowerLayerPlanCallback",
            "TileXRCcuRunDirectInstallAttempt",
            "TileXRCcuMakeRepositoryDeviceMemoryOps",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, planner)

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


if __name__ == "__main__":
    unittest.main()
