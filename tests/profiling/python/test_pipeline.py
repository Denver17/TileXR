import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from _fixtures import mapping_document, record_block, stream_id, write_capture


REPO_ROOT = Path(__file__).resolve().parents[3]


class ProfilingPipelineTests(unittest.TestCase):
    def invoke(self, root, mapping=None, blocks=None, extra_args=()):
        mapping_path, records_path = write_capture(root, mapping=mapping, blocks=blocks)
        output = root / "report"
        command = [
            sys.executable,
            "-m",
            "tilexr_profiling",
            "--mapping",
            str(mapping_path),
            "--records",
            str(records_path),
            "--output-dir",
            str(output),
        ]
        command.extend(extra_args)
        result = subprocess.run(
            command,
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        return result, output, mapping_path, records_path

    def test_complete_run_writes_exact_standard_artifacts(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, output, _, _ = self.invoke(Path(tmp))

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                {path.name for path in output.iterdir()},
                {"perfetto_trace.json", "analysis.md", "report.html"},
            )
            trace = json.loads((output / "perfetto_trace.json").read_text(encoding="utf-8"))
            self.assertEqual(trace["metadata"]["producer"], "tilexr")
            self.assertEqual(trace["metadata"]["operator"], "sample_operation")
            self.assertEqual(trace["metadata"]["status"], "complete")

    def test_incomplete_capture_writes_reports_and_returns_two(self):
        block = record_block(stream_id(0), 0, dropped_count=1, overflow=True)
        with tempfile.TemporaryDirectory() as tmp:
            result, output, _, _ = self.invoke(Path(tmp), blocks=[block])

            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertTrue((output / "perfetto_trace.json").exists())
            analysis = (output / "analysis.md").read_text(encoding="utf-8")
            self.assertIn("incomplete", analysis)
            self.assertIn(r"records\_dropped", analysis)
            self.assertIn("missing `0`, dropped `1`, overflowed `1`, excluded `0`", analysis)

    def test_invalid_mapping_returns_one_without_artifacts(self):
        mapping = mapping_document()
        mapping["schema"] = "unknown.v9"
        with tempfile.TemporaryDirectory() as tmp:
            result, output, _, _ = self.invoke(Path(tmp), mapping)

            self.assertEqual(result.returncode, 1)
            self.assertIn("unsupported mapping schema", result.stderr)
            self.assertFalse(output.exists())

    def test_nonempty_output_directory_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / "report"
            output.mkdir()
            stale = output / "stale.txt"
            stale.write_text("old generation", encoding="utf-8")

            result, _, _, _ = self.invoke(root)

            self.assertEqual(result.returncode, 1)
            self.assertIn("output directory is not empty", result.stderr)
            self.assertEqual(stale.read_text(encoding="utf-8"), "old generation")
            self.assertEqual({path.name for path in output.iterdir()}, {"stale.txt"})

if __name__ == "__main__":
    unittest.main()
