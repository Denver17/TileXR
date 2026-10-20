import json
from pathlib import Path
import tempfile
import unittest

import tilexr_profiling

from _fixtures import write_capture


class PublicPackageTests(unittest.TestCase):
    def test_public_api_generates_reports_from_explicit_paths_outside_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mapping, records = write_capture(root)
            destination = root / "reports"
            result = tilexr_profiling.generate_reports(mapping, [records], destination)
            self.assertEqual(result.status, "complete")
            self.assertEqual({path.name for path in result.artifacts},
                             {"perfetto_trace.json", "analysis.md", "report.html"})
            trace = json.loads((destination / "perfetto_trace.json").read_text(encoding="utf-8"))
            self.assertEqual(trace["metadata"]["provenance"]["analysis_version"],
                             tilexr_profiling.__version__)

    def test_canonical_package_exports_protocol_and_error_contract(self):
        self.assertEqual(tilexr_profiling.MAPPING_SCHEMA, "tilexr_profiling_mapping.v1")
        self.assertEqual(tilexr_profiling.RECORDS_SCHEMA, "tilexr_profiling_records.v1")
        self.assertTrue(issubclass(tilexr_profiling.ProfilingInputError, ValueError))

    def test_public_api_rejects_empty_non_object_attributes_before_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mapping, records = write_capture(root)
            destination = root / "reports"
            for attributes in ([], "", False, 0):
                with self.subTest(attributes=attributes):
                    with self.assertRaises(tilexr_profiling.ProfilingInputError):
                        tilexr_profiling.generate_reports(
                            mapping, [records], destination, attributes=attributes
                        )
                    self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
