#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
PUBLIC_HEADERS = [
    REPO_ROOT / "src" / "include" / "tilexr_api.h",
    REPO_ROOT / "src" / "include" / "tilexr_types.h",
    REPO_ROOT / "src" / "include" / "tilexr_collectives.h",
]
CORE_API_HEADER = REPO_ROOT / "src" / "include" / "tilexr_api.h"
COMM_WRAP = REPO_ROOT / "src" / "comm" / "comm_wrap.cpp"


class TileXRCcuPublicCommApiTest(unittest.TestCase):
    def test_core_api_header_has_no_ccu_symbols(self):
        header = CORE_API_HEADER.read_text(encoding="utf-8")
        for needle in ["CCU", "Ccu", "DirectCcu", "TILEXR_DIRECT_CCU"]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, header)

    def test_installed_public_headers_do_not_expose_low_level_ccu_model(self):
        forbidden = [
            "TileXRDirectCcu",
            "PrepareDirectCcu",
            "SubmitPrepared",
            "Repository",
            "SQE",
            " XN",
            " CKE",
            "TaskInfo",
            "rtCCULaunch",
            "rtCcuTaskInfo_t",
            "hcomm",
            "hccl",
        ]
        for path in PUBLIC_HEADERS:
            text = path.read_text(encoding="utf-8")
            for needle in forbidden:
                with self.subTest(path=path.name, needle=needle):
                    self.assertNotIn(needle, text)

    def test_collectives_header_only_exposes_high_level_backend_names(self):
        text = (REPO_ROOT / "src" / "include" / "tilexr_collectives.h").read_text(encoding="utf-8")
        for needle in [
            "TILEXR_COLLECTIVE_BACKEND_AUTO",
            "TILEXR_COLLECTIVE_BACKEND_AIV",
            "TILEXR_COLLECTIVE_BACKEND_UDMA",
            "TILEXR_COLLECTIVE_BACKEND_CCU",
        ]:
            with self.subTest(needle=needle):
                self.assertIn(needle, text)

    def test_comm_wrap_has_no_direct_ccu_public_bridge(self):
        wrapper = COMM_WRAP.read_text(encoding="utf-8")
        for needle in [
            "TileXRCommInitRankDirectCcuWithDomain",
            "TileXRCommPrepareDirectCcu",
            "TileXRCommPrepareDirectCcuMemoryCopy",
            "TileXRDirectCcuGetPreparedTask",
            "TileXRDirectCcuSubmitPrepared",
            "TileXRCommReadDirectCcuInstructions",
            "TileXRDirectCcuDestroyPrepared",
            "TileXRDirectCcuPreparedTasks",
        ]:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, wrapper)


if __name__ == "__main__":
    unittest.main()
