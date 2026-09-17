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

    def test_timeline_shows_core_work_as_nested_positive_intervals(self):
        for send_cores in (48, 40, 32):
            with self.subTest(send_cores=send_cores):
                self.check_timeline_for_split(send_cores)

    def check_timeline_for_split(self, send_cores):
        # These sequences follow the existing sender, receiver and core-63 paths.
        common = [(0, 0x53000001), (100, 0x53000002), (200, 0x53000003)]
        send = common + [(300, 0x53000300), (500, 0x53000300), (500, 0x53000300),
            (600, 0x53000004), (610, 0x53000108), (700, 0x53000109),
            (1200, 0x5300010A), (1500, 0x53000009)]
        recv_prefix = common + [(300, 0x53000005), (400, 0x53000006), (500, 0x53000007),
            (510, 0x53000100), (600, 0x53000101), (700, 0x53000102), (800, 0x53000103)]
        recv = recv_prefix + [(820, 0x53000200), (840, 0x53000201),
            (850, 0x53000202), (860, 0x53000203), (870, 0x53000204),
            (880, 0x53000206), (885, 0x53000207), (890, 0x53000205),
            (900, 0x53000208), (920, 0x53000209), (930, 0x5300020A), (940, 0x5300020B),
            (950, 0x53000104), (960, 0x53000105), (970, 0x53000106), (980, 0x53000107),
            (990, 0x53000008), (1000, 0x53000108), (1100, 0x53000109), (1200, 0x5300010A)]
        rows = {core: send if core < send_cores else recv + [(1500, 0x53000009)] for core in range(64)}
        rows[send_cores + 1] = recv_prefix + [(810, 0x53000008), (820, 0x53000108),
            (900, 0x53000109), (1200, 0x5300010A), (1500, 0x53000009)]
        rows[63] = recv + [(1250, 0x5300010B), (1300, 0x5300010C),
            (1310, 0x5300020C), (1440, 0x5300020D), (1450, 0x5300010D), (1500, 0x53000009)]
        path = write_capture(self.root / f"timeline-{send_cores}.records.v1", events_by_core=rows)
        result = measurements.breakdown_record(path, self.config, 0, CAPTURE_ID,
                                               self.root / f"timeline-{send_cores}")
        self.assertEqual(result["status"], "complete")
        trace = json.loads(Path(result["artifacts"]["perfetto_trace.json"]).read_text())
        events = [e for e in trace["traceEvents"] if e["ph"] != "M"]
        self.assertTrue(all(e["ph"] == "X" and e["dur"] > 0 for e in events))
        self.assertEqual(len({e["tid"] for e in events}), 64)
        self.assertEqual({e["args"]["core"] for e in events if e["name"] == "send_data"},
                         set(range(send_cores)))
        self.assertEqual({e["args"]["core"] for e in events if e["name"] == "recv_data"},
                         set(range(send_cores, 64)))
        for core in range(64):
            lane = [e for e in events if e["args"]["core"] == core]
            phases = ("init", "send_cnt", "send_data", "finish") if core < send_cores else (
                "init", "send_cnt", "recv_cnt", "cumsum", "prefix_consume", "recv_data", "finish")
            coarse = [next(e for e in lane if e["name"] == name) for name in phases]
            total = next(e for e in lane if e["name"] == "kernel_total")
            self.assertEqual(coarse[0]["args"]["raw_start"], total["args"]["raw_start"])
            self.assertEqual(coarse[-1]["args"]["raw_end"], total["args"]["raw_end"])
            for left, right in zip(coarse, coarse[1:]):
                self.assertEqual(left["args"]["raw_end"], right["args"]["raw_start"])
            # Overlapping slices must nest, otherwise Chrome moves them between tracks.
            ordered = sorted(lane, key=lambda e: (e["args"]["raw_start"], -e["args"]["raw_end"]))
            for i, first in enumerate(ordered):
                for second in ordered[i + 1:]:
                    self.assertFalse(first["args"]["raw_start"] < second["args"]["raw_start"]
                        < first["args"]["raw_end"] < second["args"]["raw_end"])
        sender = [e for e in events if e["args"]["core"] == 0]
        self.assertEqual([e["dur"] for e in sender if e["name"] == "packet_issue_interval"], [0.2])
        self.assertEqual([e["dur"] for e in sender if e["name"] == "send_drain_and_outputs"], [0.1])
        empty_receiver = {e["name"] for e in events if e["args"]["core"] == send_cores + 1}
        self.assertNotIn("recv_loop", empty_receiver)
        self.assertIn("recv_return", empty_receiver)
        self.assertEqual({e["args"]["core"] for e in events if e["name"] == "finish_counter"}, {63})
        receiver = {e["name"] for e in events if e["args"]["core"] == send_cores}
        self.assertTrue({"probe", "acquire_load", "metadata", "atomic", "output_issue", "relay_issue"} <= receiver)

    def test_prefetch_and_split_probe_are_positive_intervals(self):
        # Two loads are issued before either is consumed. Probe children use
        # new event IDs; the legacy acquire_load fixture above stays supported.
        recv = [(0, 0x53000001), (100, 0x53000200),
            (110, 0x53000212), (130, 0x53000213),
            (140, 0x53000214), (190, 0x53000215), (200, 0x53000201),
            (210, 0x5300020E), (220, 0x5300020F),
            (230, 0x5300020E), (240, 0x5300020F),
            (250, 0x53000210), (260, 0x53000211),
            (270, 0x53000204), (280, 0x53000205),
            (290, 0x53000208), (300, 0x53000209),
            (310, 0x53000210), (320, 0x53000211),
            (330, 0x53000204), (340, 0x53000205),
            (350, 0x53000208), (360, 0x53000209), (400, 0x53000009)]
        rows = {core: [(0, 0x53000001), (400, 0x53000009)] if core < 48 else recv for core in range(64)}
        path = write_capture(self.root / "prefetch.records.v1", events_by_core=rows)
        result = measurements.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / "prefetch")
        self.assertEqual(result["status"], "complete")
        self.assertEqual(result["diagnostic_count"], 0)
        trace = json.loads(Path(result["artifacts"]["perfetto_trace.json"]).read_text())
        events = [e for e in trace["traceEvents"] if e["ph"] != "M"]
        self.assertTrue(all(e["ph"] == "X" and e["dur"] > 0 for e in events))
        lane = [e for e in events if e["args"]["core"] == 48]
        for name, durations in {"probe": [0.1], "Probe_load_flag": [0.02], "Probe_cal_sum": [0.05],
                                "load_issue": [0.01, 0.01], "load_wait": [0.01, 0.01]}.items():
            self.assertEqual([e["dur"] for e in lane if e["name"] == name], durations)
            self.assertEqual(result["stages_us"][name]["sample_count"], len(durations) * 16)
        parent = next(e for e in lane if e["name"] == "probe")
        for child in (e for e in lane if e["name"] in ("Probe_load_flag", "Probe_cal_sum")):
            self.assertEqual(child["args"]["parent_stage_id"], parent["args"]["stage_id"])
            self.assertLess(parent["args"]["raw_start"], child["args"]["raw_start"])
            self.assertLess(child["args"]["raw_end"], parent["args"]["raw_end"])
        self.assertNotIn("acquire_load", result["stages_us"])

    def test_send_loop_and_receive_token_intervals(self):
        common = [(0, 0x53000001), (100, 0x53000002), (200, 0x53000003)]
        send = common + [(250, 0x5300021A), (450, 0x5300021B),
                         (460, 0x5300021C), (750, 0x5300021D), (800, 0x53000004)]
        recv = common + [(250, 0x53000005), (300, 0x53000006), (350, 0x53000007),
                         (400, 0x53000200), (500, 0x53000201),
                         (520, 0x5300021E), (600, 0x5300021F),
                         (620, 0x5300021E), (750, 0x5300021F), (800, 0x53000008)]
        rows = {core: (send if core < 32 else recv) + [(1000, 0x53000009)] for core in range(64)}
        path = write_capture(self.root / "coarse.records.v1", events_by_core=rows)
        result = measurements.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / "coarse")
        self.assertEqual(result["status"], "complete")
        self.assertEqual(result["diagnostic_count"], 0)
        trace = json.loads(Path(result["artifacts"]["perfetto_trace.json"]).read_text())
        events = [e for e in trace["traceEvents"] if e["ph"] == "X"]
        for core, expected in ((0, {"send_first_hop": [0.2], "send_direct": [0.29],
                                   "send_prepare": [0.05], "send_drain_and_outputs": [0.05]}),
                               (32, {"probe": [0.1], "recv_token": [0.08, 0.13]})):
            lane = [e for e in events if e["args"]["core"] == core]
            for name, durations in expected.items():
                self.assertEqual([e["dur"] for e in lane if e["name"] == name], durations)
        self.assertEqual(result["stages_us"]["recv_token"]["sample_count"], 64)
        for name in ("packet_issue_interval", "load_issue", "load_wait", "metadata", "output_issue", "relay_issue"):
            self.assertFalse(any(e["name"] == name for e in events))

    def test_count_filter_and_send_intervals_on_active_cores(self):
        for first_core in (0, 48):
            with self.subTest(first_core=first_core):
                rows = {}
                for core in range(64):
                    row = [(0, 0x53000001), (100, 0x53000002)]
                    if core >= first_core:
                        row += [(200, 0x53000216), (400, 0x53000217),
                                (410, 0x53000218), (700, 0x53000219)]
                    row += [(800, 0x53000003), (1000, 0x53000009)]
                    rows[core] = row
                path = write_capture(self.root / f"count-{first_core}.records.v1", events_by_core=rows)
                result = measurements.breakdown_record(path, self.config, 0, CAPTURE_ID,
                                                       self.root / f"count-{first_core}")
                self.assertEqual(result["status"], "complete")
                self.assertEqual(result["diagnostic_count"], 0)
                trace = json.loads(Path(result["artifacts"]["perfetto_trace.json"]).read_text())
                for name, duration in (("send_cnt_filter", 0.2), ("send_cnt_send", 0.29)):
                    self.assertEqual(result["stages_us"][name]["sample_count"], 64 - first_core)
                    samples = [e for e in trace["traceEvents"] if e["name"] == name]
                    self.assertEqual({e["args"]["core"] for e in samples}, set(range(first_core, 64)))
                    self.assertTrue(all(e["ph"] == "X" and e["dur"] == duration for e in samples))
                    parents = {e["args"]["core"]: e for e in trace["traceEvents"] if e["name"] == "send_cnt"}
                    for sample in samples:
                        parent = parents[sample["args"]["core"]]
                        self.assertLess(parent["args"]["raw_start"], sample["args"]["raw_start"])
                        self.assertLess(sample["args"]["raw_end"], parent["args"]["raw_end"])
        # Older producers have no split records; no synthetic timings are added.
        for row in self.records:
            self.assertNotIn("send_cnt_filter", row["breakdown"]["stages_us"])
            self.assertNotIn("send_cnt_send", row["breakdown"]["stages_us"])

    def test_probe_includes_idle_scans_without_substage_records(self):
        # All unsuccessful scans and the successful batch share this interval.
        recv = [(0, 0x53000001), (100, 0x53000200), (10100, 0x53000201),
                (10200, 0x5300020E), (10300, 0x5300020F), (10400, 0x53000009)]
        rows = {core: [(0, 0x53000001), (10400, 0x53000009)] if core < 48 else recv
                for core in range(64)}
        path = write_capture(self.root / "coalesced-probe.records.v1", events_by_core=rows)
        result = measurements.breakdown_record(path, self.config, 0, CAPTURE_ID,
                                               self.root / "coalesced-probe")
        self.assertEqual(result["status"], "complete")
        self.assertEqual(result["diagnostic_count"], 0)
        self.assertEqual(result["stages_us"]["probe"]["sample_count"], 16)
        self.assertNotIn("Probe_load_flag", result["stages_us"])
        self.assertNotIn("Probe_cal_sum", result["stages_us"])
        trace = json.loads(Path(result["artifacts"]["perfetto_trace.json"]).read_text())
        probes = [e for e in trace["traceEvents"] if e["name"] == "probe"]
        self.assertEqual(len(probes), 16)
        self.assertTrue(all(e["ph"] == "X" and e["dur"] == 10.0 for e in probes))

    def test_pingpong_finish_returns_after_counter_reset_without_handshake(self):
        rows = {core: [(0, 0x53000001), (100, 0x53000108),
                      (200, 0x53000109), (300, 0x5300010A)] for core in range(64)}
        rows[63].append((400, 0x5300010B))
        for row in rows.values():
            row.append((500, 0x53000009))
        path = write_capture(self.root / "pingpong.records.v1", events_by_core=rows)
        result = measurements.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / "pingpong")
        trace = json.loads(Path(result["artifacts"]["perfetto_trace.json"]).read_text())
        events = [event for event in trace["traceEvents"] if event["ph"] == "X"]
        self.assertFalse({"finish_publish", "finish_wait"} & {event["name"] for event in events})
        returns = [event for event in events if event["name"] == "finish_return"]
        self.assertEqual(len(returns), 64)
        for event in returns:
            self.assertAlmostEqual(event["dur"], 0.1 if event["args"]["core"] == 63 else 0.2)

    def test_prefix_index_finish_has_no_counter_or_atomic_samples(self):
        rows = {core: [(0, 0x53000001), (100, 0x53000108),
                      (200, 0x53000109), (300, 0x5300010A),
                      (500, 0x53000009)] for core in range(64)}
        path = write_capture(self.root / "prefix.records.v1", events_by_core=rows)
        result = measurements.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / "prefix")
        trace = json.loads(Path(result["artifacts"]["perfetto_trace.json"]).read_text())
        events = [event for event in trace["traceEvents"] if event["ph"] == "X"]
        self.assertFalse({"finish_counter", "atomic"} & {event["name"] for event in events})
        returns = [event for event in events if event["name"] == "finish_return"]
        self.assertEqual(len(returns), 64)
        for event in returns:
            self.assertAlmostEqual(event["dur"], 0.2)

    def test_local_finish_without_sync_preserves_return_for_all_cores(self):
        rows = {core: [(0, 0x53000001), (100, 0x53000108),
                      (200, 0x53000109), (500 + core, 0x53000009)]
                for core in range(64)}
        path = write_capture(self.root / "local-finish.records.v1", events_by_core=rows)
        result = measurements.breakdown_record(path, self.config, 0, CAPTURE_ID,
                                               self.root / "local-finish")
        self.assertEqual(result["status"], "complete")
        self.assertEqual(result["diagnostic_count"], 0)
        trace = json.loads(Path(result["artifacts"]["perfetto_trace.json"]).read_text())
        events = [event for event in trace["traceEvents"] if event["ph"] == "X"]
        names = {event["name"] for event in events}
        self.assertFalse({"finish_sync", "finish_counter", "finish_publish", "finish_wait"} & names)
        returns = [event for event in events if event["name"] == "finish_return"]
        self.assertEqual(len(returns), 64)
        for event in returns:
            self.assertAlmostEqual(event["dur"], (300 + event["args"]["core"]) / 1000)
        self.assertEqual(result["stages_us"]["finish_local"]["sample_count"], 64)

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
            self.assertTrue(any(e["name"] == "init" and e["ph"] == "X" for e in trace["traceEvents"]))
            self.assertFalse(any(e["ph"] == "i" and e.get("cat") != "diagnostic"
                                 for e in trace["traceEvents"]))

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
