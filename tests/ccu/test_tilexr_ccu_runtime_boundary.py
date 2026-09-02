#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
CCU_HEADER = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_runtime.h"
CCU_SOURCE = REPO_ROOT / "src" / "comm" / "ccu" / "tilexr_ccu_runtime.cpp"
COMM_CMAKE = REPO_ROOT / "src" / "comm" / "CMakeLists.txt"


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
    "COMM_ENGINE_CCU",
    "COMM_PROTOCOL_UBC_CTP",
    "HCCL_SERVER_TYPE_CCU",
    "RT_RES_TYPE_CCU_CKE",
    "RT_RES_TYPE_CCU_XN",
    "dlopen",
    "dlsym",
]


class TileXRCcuRuntimeBoundaryTest(unittest.TestCase):
    def test_runtime_submit_wrapper_is_wired_into_tile_comm(self):
        cmake = COMM_CMAKE.read_text(encoding="utf-8")
        self.assertIn("ccu/tilexr_ccu_runtime.h", cmake)
        self.assertIn("ccu/tilexr_ccu_runtime.cpp", cmake)

    def test_runtime_submit_wrapper_uses_public_runtime_abi_only(self):
        header = CCU_HEADER.read_text(encoding="utf-8")
        source = CCU_SOURCE.read_text(encoding="utf-8")

        self.assertIn("TILEXR_CCU_SQE_ARGS_LEN", header)
        self.assertIn("struct TileXRCcuTask", header)
        self.assertIn("TileXRCcuValidateTask", header)
        self.assertIn("TileXRCcuSubmitTask", header)
        self.assertNotIn("runtime/kernel.h", header)
        self.assertNotIn("rtCcuTaskInfo_t", header)

        self.assertIn("#include <runtime/kernel.h>", source)
        self.assertIn("static_assert(RT_CCU_SQE_ARGS_LEN == TILEXR_CCU_SQE_ARGS_LEN", source)
        self.assertIn("rtCcuTaskInfo_t runtimeTask", source)
        self.assertIn("rtCCULaunch(&runtimeTask, stream)", source)
        self.assertIn("stream == nullptr", source)
        self.assertIn("RT_CCU_INST_CNT_INVALID", source)
        self.assertIn("RT_CCU_INST_START_MAX", source)
        self.assertIn("task.argSize != 1 && task.argSize != TILEXR_CCU_SQE_ARGS_LEN", source)
        self.assertNotIn("TILEXR_CCU_DIRECT_TASK_DIE_ID", source)
        self.assertNotIn("TILEXR_CCU_DIRECT_TASK_TIMEOUT", source)
        self.assertNotIn("TILEXR_CCU_DIRECT_TASK_ARG_SIZE", source)
        self.assertNotIn("TILEXR_CCU_DIRECT_TASK_ARG", source)
        self.assertNotIn("ApplyRuntimeTaskOverrides", source)
        self.assertIn("TILEXR_ERROR_MKIRT", source)

        combined = header + "\n" + source
        for needle in PRIVATE_CCU_PRODUCER_NEEDLES:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, combined)

    def test_rt_ccu_launch_reference_is_confined_to_runtime_submit_wrapper(self):
        offenders = []
        for root in [REPO_ROOT / "src" / "comm", REPO_ROOT / "src" / "include"]:
            for path in root.rglob("*"):
                if path.is_file() and path.suffix in {".h", ".hpp", ".cpp", ".cc", ".c"}:
                    text = path.read_text(encoding="utf-8", errors="replace")
                    if "rtCCULaunch" in text and path != CCU_SOURCE:
                        offenders.append(path.relative_to(REPO_ROOT).as_posix())
        self.assertEqual([], offenders)

    def test_runtime_kernel_header_is_confined_to_runtime_submit_wrapper(self):
        offenders = []
        for root in [REPO_ROOT / "src" / "comm", REPO_ROOT / "src" / "include"]:
            for path in root.rglob("*"):
                if path.is_file() and path.suffix in {".h", ".hpp", ".cpp", ".cc", ".c"}:
                    text = path.read_text(encoding="utf-8", errors="replace")
                    if "runtime/kernel.h" in text and path != CCU_SOURCE:
                        offenders.append(path.relative_to(REPO_ROOT).as_posix())
        self.assertEqual([], offenders)


if __name__ == "__main__":
    unittest.main()
