"""Installed-package behavior replaces source-token dependency guards."""
import importlib
import unittest


class ProfilingDependencyTests(unittest.TestCase):
    def test_all_public_modules_import_without_cann(self):
        for name in ("analysis", "cli", "html_report", "io", "mapping", "model",
                     "pairing", "perfetto", "records", "provenance"):
            self.assertIsNotNone(importlib.import_module("tilexr_profiling." + name))


if __name__ == "__main__":
    unittest.main()
