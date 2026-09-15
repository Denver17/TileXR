"""Final-launch captures, local reports and coordinator collection; no NPU."""
import json
from pathlib import Path
import tempfile
import os
import shutil
import subprocess
import sys
import unittest
from unittest import mock
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
        self.records = [self.capture(rank, duration) for rank, duration in enumerate((9000, 1000, 4000, 4000))]

    def capture(self, rank, duration=8000, fault="", capture_id=CAPTURE_ID):
        directory = self.root / f"profile-rank-{rank}"
        if directory.exists():
            shutil.rmtree(directory)
        path = write_capture(self.root / f"rank-{rank}.records.v1", rank=rank, duration=duration,
                             fault=fault, capture_id=capture_id)
        capture = measurements.breakdown_record(path, self.config, rank, capture_id,
                                               directory)
        return dict(rank=rank, breakdown=capture)

    def collect(self, records=None):
        return measurements.collect_breakdown_reports(self.records if records is None else records,
            self.config, self.output, "run-test", "a" * 64)

    def test_coordinator_uses_finished_reports_without_raw_parsing_or_rendering(self):
        for row in self.records:
            Path(row["breakdown"]["records_path"]).unlink()
        with mock.patch.object(measurements, "load_profile_run", side_effect=AssertionError("central raw parse")), \
                mock.patch.object(measurements, "generate_reports_for_run", side_effect=AssertionError("central render")):
            result = self.collect()
        self.assertEqual(result["status"], "complete")
        self.assertEqual(result["selected_ranks"], dict(fastest=1, p50=2, slowest=0))
        self.assertEqual(result["kernel_us"], dict(median=4, minimum=1, maximum=9, mean=4.5))
        self.assertEqual(len(result["ranks"]), 4)
        self.assertNotIn("all_ranks", result)
        directory = Path(result["trace_dir"])
        self.assertEqual(result["report"], str(directory))
        self.assertEqual({path.name for path in directory.iterdir()}, {f"rank-{rank}.json" for rank in range(4)})
        for row in self.records:
            copied = Path(result["ranks"][str(row["rank"])])
            source = Path(row["breakdown"]["artifacts"]["perfetto_trace.json"])
            self.assertEqual(copied.parent, directory)
            self.assertEqual(copied.read_bytes(), source.read_bytes())
        self.assertTrue(all(Path(path).parent == directory for path in result["selected"].values()))
        self.assertFalse(result["raw_records_collected"])
        self.assertEqual(json.loads(Path(result["index"]).read_text()), result)
        for row in self.records:
            measurements.validate_rank_measurements(row, self.config)
            capture = row["breakdown"]
            self.assertEqual(capture["measured_launches"], 1)
            self.assertEqual(capture["captured_launch"], 4)
            self.assertEqual(capture["selection"], "last_launch")
            self.assertNotIn("init_end", capture["stages_us"])
            self.assertEqual(capture["stages_us"]["atomic"]["sample_count"], 64)
            trace = json.loads(Path(capture["artifacts"]["perfetto_trace.json"]).read_text())
            events = [e for e in trace["traceEvents"] if e["ph"] == "X" and e["name"] == "kernel_total"]
            self.assertEqual(len(events), 64)
            self.assertEqual({e["dur"] for e in events}, set(capture["sample_us"]))

    def test_local_capture_is_parsed_once(self):
        with mock.patch.object(measurements, "load_profile_run", wraps=measurements.load_profile_run) as reader:
            self.capture(0)
        self.assertEqual(reader.call_count, 1)

    def test_old_chrome_display_unit_is_removed_without_changing_events(self):
        source = self.root / "legacy-trace.json"
        trace = dict(displayTimeUnit="us", traceEvents=[dict(name="stage", ph="X", pid=0, tid=0,
            ts=1.25, dur=2.5, args=dict(displayTimeUnit="us"))])
        measurements.atomic_write_json(source, trace)
        original = source.read_bytes()
        copied = Path(measurements.copy_rank_trace(source, self.output, 0))
        exported = json.loads(copied.read_text())
        self.assertNotIn("displayTimeUnit", exported)
        self.assertEqual(exported["traceEvents"], trace["traceEvents"])
        self.assertEqual(source.read_bytes(), original)

    def test_even_rank_median_averages_middle_values(self):
        self.records[3] = self.capture(3, 6000)
        result = self.collect(list(reversed(self.records)))
        self.assertEqual(result["kernel_us"]["median"], 5)
        self.assertEqual(result["selected_ranks"]["p50"], 2)
        summary = measurements.summarize_measurements(self.records, self.config, "a" * 64)
        self.assertEqual(summary["breakdown"]["kernel_us"]["median"], 5)
        self.assertEqual(summary["breakdown"]["rank_us"], [9, 1, 4, 6])
        self.assertIn("最后一轮", measurements.render_measurements(summary))
        self.assertNotIn("测量轮次", measurements.render_measurements(summary))

    def test_collection_rebases_reports_and_preserves_remote_raw_location(self):
        evidence = self.records[0]["breakdown"]
        node = self.root / "collected nodes/1"
        shutil.copytree(Path(evidence["summary_path"]).parent, node / "profile-rank-0")
        relocated = measurements.relocate_breakdown_report(evidence, node, 0, "192.0.2.2")
        shutil.rmtree(Path(evidence["summary_path"]).parent)
        self.records[0]["breakdown"] = relocated
        result = self.collect()
        self.assertEqual(result["status"], "complete")
        self.assertFalse(relocated["records_collected"])
        self.assertIsNone(relocated["local_records_path"])
        self.assertEqual(relocated["records_path"], evidence["records_path"])
        self.assertEqual(result["reports"]["0"]["raw_records"],
                         dict(host="192.0.2.2", path=evidence["records_path"], local_path=None))
        self.assertEqual(Path(result["ranks"]["0"]).read_bytes(),
                         (node / "profile-rank-0/perfetto_trace.json").read_bytes())

    def test_missing_corrupt_artifact_and_summary_retain_other_reports(self):
        for fault in ("missing", "corrupt", "summary", "scalar"):
            with self.subTest(fault=fault):
                self.records[0] = self.capture(0)
                evidence = self.records[0]["breakdown"]
                artifact = Path(evidence["artifacts"]["perfetto_trace.json"])
                if fault == "missing":
                    artifact.unlink()
                elif fault == "corrupt":
                    artifact.write_bytes(b"!" + artifact.read_bytes()[1:])
                elif fault == "summary":
                    Path(evidence["summary_path"]).write_text("{}")
                else:
                    evidence["sample_us"] = [123]
                result = self.collect()
                self.assertEqual(result["status"], "incomplete")
                self.assertEqual(result["available_ranks"], 3)
                self.assertFalse(result["reports"]["0"]["artifacts_verified"])
                self.assertEqual(set(result["ranks"]), {"1", "2", "3"})
                self.assertTrue(Path(result["report"]).is_dir())
                self.assertFalse((Path(result["trace_dir"]) / "rank-0.json").exists())

    def test_missing_and_overflow_ranks_keep_partial_reports(self):
        self.records[0] = self.capture(0, fault="overflow")
        result = self.collect(self.records[:-1])
        self.assertEqual(result["available_ranks"], 2)
        self.assertEqual(result["reports"]["0"]["status"], "incomplete")
        self.assertTrue(result["reports"]["0"]["artifacts_verified"])
        self.assertEqual(result["reports"]["3"]["status"], "unavailable")
        self.assertIn("0", result["ranks"])
        self.assertNotIn("3", result["ranks"])
        self.assertEqual(result["rank_us"], {"1": 1, "2": 4})

    def test_mixed_capture_uuid_prevents_cross_run_statistics(self):
        self.records[0] = self.capture(0, capture_id="563acd67-e125-46c6-bd57-69ca989951dd")
        result = self.collect()
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["available_ranks"], 3)
        self.assertNotIn("0", result["rank_us"])
        self.assertIn("does not belong", result["reports"]["0"]["error"])

    def test_accumulating_overflow_capture_and_rank_identity(self):
        for fault in ("accumulating", "overflow", "capture", "rank"):
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

    def test_explicit_offline_reprocessing_and_cli_use_copied_raw_directory(self):
        offline = self.root / "offline"
        (offline / "chrome").mkdir(parents=True)
        (offline / "chrome/rank-99.json").write_text("{}")
        for row in self.records:
            row["breakdown"]["records_collected"] = False
        with self.assertRaisesRegex(ValueError, "--records-dir"):
            measurements.export_breakdown_traces(self.records, self.config, offline, "run-test", "a" * 64)
        summary = self.root / "summary.json"
        # A parse/report failure can leave no breakdown summary, even when raw
        # data remains usable for an explicit offline retry.
        self.records[0].pop("breakdown")
        summary.write_text(json.dumps(dict(nodes=[dict(reported_result=dict(ranks=self.records))],
            chain_configuration=self.config, rank_size=4, run_id="run-test", traces=dict(kernel_sha256="a" * 64))))
        command = subprocess.run([sys.executable, measurements.__file__, str(summary),
            "--records-dir", str(self.root), "--output-dir", str(offline)],
            capture_output=True, text=True, timeout=20)
        self.assertEqual(command.returncode, 0, command.stdout + command.stderr)
        self.assertFalse((offline / "all-ranks").exists())
        self.assertEqual({p.name for p in (offline / "chrome").iterdir()},
                         {f"rank-{rank}.json" for rank in range(4)})
        for rank in range(4):
            trace = json.loads((offline / f"chrome/rank-{rank}.json").read_text())
            events = [e for e in trace["traceEvents"] if e["ph"] == "X" and e["name"] == "kernel_total"]
            self.assertEqual(len(events), 64)
            self.assertEqual({e["args"]["rank"] for e in events}, {rank})

    def test_cli_collects_retained_traces_without_raw_captures(self):
        for row in self.records:
            Path(row["breakdown"]["records_path"]).unlink()
        summary = self.root / "summary.json"
        summary.write_text(json.dumps(dict(nodes=[dict(reported_result=dict(ranks=self.records))],
            chain_configuration=self.config, rank_size=4, run_id="run-test", traces=dict(kernel_sha256="a" * 64))))
        command = subprocess.run([sys.executable, measurements.__file__, str(summary),
            "--output-dir", str(self.output)], capture_output=True, text=True, timeout=20)
        self.assertEqual(command.returncode, 0, command.stdout + command.stderr)
        self.assertEqual({p.name for p in (self.output / "chrome").iterdir()},
                         {f"rank-{rank}.json" for rank in range(4)})

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
            "--launch-count", "1", "--expected-launch-count", "1"],
            cwd=installed, env=env, capture_output=True, text=True, timeout=20)
        self.assertEqual(command.returncode, 0, command.stdout + command.stderr)
        trace = json.loads((output / "perfetto_trace.json").read_text())
        intervals = [e for e in trace["traceEvents"] if e["ph"] == "X" and e["name"] == "kernel_total"]
        self.assertEqual(len(intervals), 64)

    def test_capacity_is_fixed_for_large_launch_batches_and_invalid_inputs_fail(self):
        for loops, warmup in ((3, 2), (1024, 0), (100000, 99999)):
            self.assertEqual(measurements.capture_capacity(loops, warmup), 65536)
        for loops, warmup in ((0, 0), (1, -1), (True, 0), (1, 0.5)):
            with self.subTest(loops=loops, warmup=warmup), self.assertRaises(ValueError):
                measurements.capture_capacity(loops, warmup)
        with self.assertRaises(ValueError):
            measurements.breakdown_record(self.root / "missing", self.config, 0, CAPTURE_ID, self.root / "missing-out")


if __name__ == "__main__":
    unittest.main()
