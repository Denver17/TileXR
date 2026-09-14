"""Standard records capture, launch selection and report integration; no NPU."""
import json
from pathlib import Path
import tempfile
import os
import shutil
import subprocess
import sys
import unittest
import measurements
from test_profiling_fixture import CAPTURE_ID, write_capture


class TraceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / "traces"
        self.config = dict(run_mode="breakdown", loops=3, warmup=2, rank_size=4,
                           bs=256, h=7168, topk=6, moe_expert_num=384, dtype="bf16", block_dim="auto")
        self.records = []
        for rank, duration in enumerate((9000, 1000, 4000, 4000)):
            path = write_capture(self.root / f"rank-{rank}.records.v1", rank=rank, duration=duration)
            capture = measurements.breakdown_record(path, self.config, rank, CAPTURE_ID,
                                                   self.root / f"report-rank-{rank}")
            self.records.append(dict(rank=rank, breakdown=capture))

    def export(self):
        return measurements.export_breakdown_traces(self.records, self.config, self.output, "run-test", "a" * 64)

    def test_standard_reports_keep_every_core_and_measured_launch(self):
        result = self.export()
        self.assertEqual(result["status"], "complete")
        self.assertEqual(result["selected_ranks"], dict(fastest=1, p50=2, slowest=0))
        self.assertEqual(len(result["ranks"]), 4)
        self.assertTrue(Path(result["report"]).is_file())
        combined = json.loads(Path(result["all_ranks"]).read_text())
        events = [e for e in combined["traceEvents"] if e["ph"] == "X" and e["name"] == "kernel_total"]
        self.assertEqual(len(events), 4 * 64 * 3)
        self.assertEqual(sorted(set(e["dur"] for e in events)), [1, 4, 9])
        for row in self.records:
            measurements.validate_rank_measurements(row, self.config)
            self.assertEqual(row["breakdown"]["measured_launches"], 3)
            self.assertNotIn("init_end", row["breakdown"]["stages_us"])
            self.assertEqual(row["breakdown"]["stages_us"]["atomic"]["sample_count"], 64 * 3)

    def test_stale_overflow_capture_and_rank_identity(self):
        for fault in ("stale", "overflow", "capture", "rank"):
            path = write_capture(self.root / (fault + ".records.v1"), fault=fault)
            if fault in ("capture", "rank"):
                with self.subTest(fault=fault), self.assertRaises(ValueError):
                    measurements.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / fault)
                continue
            capture = measurements.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / fault)
            self.assertEqual(capture["status"], "incomplete")
            self.assertTrue((self.root / fault / "report.html").is_file())
            with self.assertRaisesRegex(ValueError, "incomplete"):
                measurements.validate_rank_measurements(dict(rank=0, breakdown=capture), self.config)

    def test_installed_generic_package_and_mapping_work_without_source_tree(self):
        installed = self.root / "install/share/tilexr"
        tools = installed / "tools"
        tools.mkdir(parents=True)
        shutil.copytree(Path(measurements.__file__).resolve().parents[1] / "profiling", tools / "profiling",
                        ignore=shutil.ignore_patterns("__pycache__"))
        mapping = installed / "profiling/server_dedup.mapping.json"
        mapping.parent.mkdir()
        shutil.copy2(measurements.profiling_mapping(), mapping)
        output = self.root / "installed-report"
        env = dict(os.environ, PYTHONPATH=str(tools))
        command = subprocess.run([sys.executable, "-m", "profiling", "--mapping", str(mapping),
            "--records", self.records[0]["breakdown"]["records_path"], "--output-dir", str(output),
            "--skip-launches", "2", "--launch-count", "3", "--expected-launch-count", "5"],
            cwd=installed, env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(command.returncode, 0, command.stdout + command.stderr)
        self.assertTrue((output / "report.html").is_file())
        trace = json.loads((output / "perfetto_trace.json").read_text())
        intervals = [e for e in trace["traceEvents"] if e["ph"] == "X" and e["name"] == "kernel_total"]
        self.assertEqual(len(intervals), 64 * 3)

    def test_capacity_arithmetic_and_missing_capture(self):
        self.assertEqual(measurements.capture_capacity(3, 2), 5 * 65536)
        self.assertEqual(measurements.capture_capacity(1023, 0), 1023 * 65536)
        for loops, warmup in ((1024, 0), (1023, 1), (0, 0), (1, -1)):
            with self.subTest(loops=loops, warmup=warmup), self.assertRaises(ValueError):
                measurements.capture_capacity(loops, warmup)
        with self.assertRaises(ValueError):
            measurements.breakdown_record(self.root / "missing", self.config, 0, CAPTURE_ID, self.root / "missing-out")


if __name__ == "__main__":
    unittest.main()
