#!/usr/bin/env python3
#
# Copyright (c) 2026 TileXR Project
#

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


class TileXRCcuPublicApiCompileProbeTest(unittest.TestCase):
    def test_external_direct_ccu_public_probe_removed(self):
        self.assertFalse((REPO_ROOT / "tests" / "ccu" / "ccu_public_direct_api_compile_probe.c").exists())


if __name__ == "__main__":
    unittest.main()
