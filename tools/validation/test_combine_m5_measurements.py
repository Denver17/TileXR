"""M5 module-only trace roles and multi-launch logical versus wire traffic."""
import copy
import csv
import json
from pathlib import Path
import tempfile
import unittest

import measurements as measure
import operator_measurements as operators
from test_profiling_fixture import CAPTURE_ID, write_capture


class M5MeasurementTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="tilexr-m5-profile-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.config = dict(run_mode="breakdown", operator_mode="combine", rank_size=16, loops=5, warmup=2,
                           implemented_through="M5", scope="full", combine_send_core_num=32,
                           bs=96, h=516, topk=6, moe_expert_num=96, dtype="bf16", block_dim="auto")
        self.row = dict(rank=0, combine_type3_rows=2, combine_type1_rows=3, combine_token_num=6,
                        combine_algorithm_bytes=5160, combine_stage_copy_bytes=4128,
                        combine_direct_payload_bytes=3096, combine_direct_wire_bytes=4608,
                        combine_control_publish_bytes=24576)
        self.sha = "c" * 64
        self.row.update(combine_type2_rows=1, combine_partial_payload_bytes=1032,
                        combine_partial_wire_bytes=1536, combine_partial_self_read_bytes=1032,
                        combine_partial_member_rows=2, combine_partial_local_members=1, combine_partial_stage_read_bytes=2064, combine_algorithm_bytes=6192)
        self.row.update(combine_direct_received_rows=95, combine_partial_received_rows=1,
                        combine_final_read_bytes=96 * 1032, combine_y_out_bytes=96 * 1032,
                        combine_flag_clear_bytes=96 * 6 * 3 * 32, combine_algorithm_bytes=6192 + 96 * 1032)
        self.mapping = Path(__file__).resolve().parents[2] / "src/ep/profiling/server_dedup_combine.mapping.json"

    def events(self):
        events = {}
        for core in range(64):
            reader = core >= self.config["combine_send_core_num"]
            pairs = [0x3003, 0x3017, 0x3019 if reader else 0x301b]
            if reader:
                pairs.extend((0x301d, 0x301f))
            pairs.append(0x3013)
            rows = [(0, 0x3001)]
            for index, event in enumerate(pairs, 1):
                rows.extend(((index * 1000, event), (index * 1000 + 500, event + 1)))
            rows.append((7000, 0x3002))
            events[core] = rows
        return events

    def capture(self, events=None, suffix="base"):
        path = write_capture(self.root / (suffix + ".records.v1"), launches=self.config["loops"] + self.config["warmup"], warmup=self.config["warmup"],
                             events_by_core=events or self.events())
        return operators.breakdown_record(path, self.config, 0, CAPTURE_ID, self.root / suffix,
                                          operator="combine", run_id="run-test", kernel_sha=self.sha, record=self.row, mapping_path=self.mapping)

    def validate(self, evidence):
        row = dict(self.row, operator_measurements=dict(combine=dict(breakdown=evidence)))
        operators.validate_operator_measurements(row, self.config, run_id="run-test", kernel_shas=dict(combine=self.sha))

    def test_modules_roles_and_no_delay(self):
        evidence = self.capture()
        self.validate(evidence)
        self.assertEqual({name: item["sample_count"] for name, item in evidence["stages_us"].items()},
                         operators.return_stage_counts(self.config))
        self.assertEqual(evidence["wait_us"], 0)
        self.assertEqual(evidence["direct_wire_bytes"], 4608)
        self.assertEqual(evidence["stage_payload_bytes"], 2064)
        self.assertEqual(evidence["captured_launch"], 6)
        self.assertEqual(evidence["last_launch"], 6)
        self.assertEqual(evidence["scope"], "full")
        self.assertEqual(evidence["final_read_bytes"], 96 * 1032)
        for fault in ("wrong-role", "missing-module", "missing-final", "missing-end"):
            events = self.events()
            if fault == "wrong-role":
                events[0], events[32] = events[32], events[0]
            elif fault == "missing-module":
                events[0] = [item for item in events[0] if item[1] not in (0x301b, 0x301c)]
            elif fault == "missing-final":
                events[32] = [item for item in events[32] if item[1] not in (0x301f, 0x3020)]
            else:
                events[0].pop()
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                self.validate(self.capture(events, fault))
        for key, value in (("last_launch", 0), ("captured_launch", 0), ("scope", "partial"), ("wait_us", 50), ("direct_wire_bytes", 0), ("direct_payload_bytes", 0),
                           ("partial_payload_bytes", 0), ("partial_stage_read_bytes", 0), ("partial_wire_bytes", 0), ("final_read_bytes", 0), ("y_out_bytes", 0), ("flag_clear_bytes", 0), ("stage_copy_bytes", 0), ("implemented_through", "M2")):
            bad = copy.deepcopy(evidence); bad[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(bad)

    def test_final_uses_recv_cores_for_asymmetric_splits(self):
        for send in (16, 24, 40, 48, 56):
            with self.subTest(send=send):
                self.config["combine_send_core_num"] = send
                self.row["combine_control_publish_bytes"] = (64 * 8 + 8 * (64 - send)) * 32
                evidence = self.capture(suffix="send-" + str(send))
                self.validate(evidence)
                self.assertEqual(evidence["stages_us"]["m5_final"]["sample_count"], 64 - send)
                self.assertEqual(evidence["stages_us"]["m4_partial"]["sample_count"], 64 - send)
                events = self.events()
                # Move only M5 back to a Send core; reject the old role rule.
                final = [item for item in events[send] if item[1] in (0x301f, 0x3020)]
                events[send] = [item for item in events[send] if item not in final]
                events[0] = sorted(events[0] + final)
                with self.assertRaises(ValueError):
                    self.capture(events, "bad-send-" + str(send))

    def test_token_issue_intervals_and_m3_completion_trace(self):
        events = self.events()
        for core, rows in events.items():
            if core < 32:
                rows.append((3450, 0x3021))
            else:
                for task in range(3):
                    rows.extend(((5010 + task * 100, 0x3024), (5050 + task * 100, 0x3025)))
            if core == 32:
                rows.extend(((4010, 0x3022), (4050, 0x3023)))
            rows.sort()
        evidence = self.capture(events, "tokens")
        self.validate(evidence)
        self.assertEqual(evidence["stages_us"]["m4_token_issue"]["sample_count"], 1)
        self.assertEqual(evidence["stages_us"]["m5_token_issue"]["sample_count"], 96)
        trace = json.loads(Path(evidence["artifacts"]["perfetto_trace.json"]).read_text())["traceEvents"]
        issued = [e for e in trace if e["name"].startswith("m5_token_issue[") and e["ph"] == "X"]
        self.assertEqual(len(issued), 96)
        for core in range(32, 64):
            tasks = [e for e in issued if e["args"]["core"] == core]
            self.assertEqual([e["name"] for e in tasks], ["m5_token_issue[%d]" % t for t in range(3)])
            self.assertEqual(len({e["tid"] for e in tasks}), 1)  # one row, successive tasks
            self.assertEqual([e["args"]["attributes"]["task_sequence"] for e in tasks], list(range(3)))
        partial_tokens = [e for e in trace if e["name"].startswith("m4_token_issue[") and e["ph"] == "X"]
        self.assertEqual([e["name"] for e in partial_tokens], ["m4_token_issue[0]"])
        partial_stages = [e for e in trace if e["name"] == "m4_partial" and e["ph"] == "X"]
        self.assertEqual(len(partial_stages), 32)
        self.assertTrue(all(e["args"]["attributes"]["mte3_completion_required"] is False for e in partial_stages))
        self.assertTrue(all("MTE3 may remain in flight" in e["args"]["attributes"]["timing"] for e in partial_stages))
        markers = [e for e in trace if e["name"] == "m3_mte3_complete"]
        self.assertEqual(len(markers), 32)
        self.assertTrue(all(e["ph"] == "i" for e in markers))
        for fault in ("missing-end", "missing-task", "wrong-role", "outside-parent"):
            bad = copy.deepcopy(events)
            if fault == "missing-end":
                bad[32].remove((5050, 0x3025))
            elif fault == "missing-task":
                bad[32].remove((5010, 0x3024)); bad[32].remove((5050, 0x3025))
            elif fault == "wrong-role":
                bad[0].extend(((3100, 0x3024), (3150, 0x3025)))
            else:
                bad[32].extend(((5600, 0x3024), (5700, 0x3025)))
            bad = {core: sorted(rows) for core, rows in bad.items()}
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                self.validate(self.capture(bad, fault + "-token"))

    def test_multilaunch_statistics_distinguish_per_launch_and_total_bytes(self):
        path = self.root / "PROF_0/mindstudio_profiler_output/op_summary.csv"
        path.parent.mkdir(parents=True)
        with path.open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("Op Name", "Task Start Time(us)", "Task Duration(us)"))
            for launch in range(7):
                writer.writerow((measure.kernel_name("combine"), 1000 + launch * 100, 70 if launch < 2 else 7))
        config = dict(self.config, run_mode="performance")
        rows = []
        for rank in range(16):
            row = dict(self.row, rank=rank)
            evidence = operators.performance_record(path.parent.parent, 5, 2, operator="combine", run_id="run-test",
                                                    rank=rank, kernel_sha=self.sha, config=config, record=row)
            self.assertEqual(evidence["sample_us"], [7] * 5)
            rows.append(dict(row, operator_measurements=dict(combine=dict(performance=evidence))))
        summary = operators.summarize_operator_measurements(rows, config, "run-test", dict(combine=self.sha))["combine"]
        self.assertEqual(summary["direct_wire_bytes"], 4608 * 16)
        self.assertEqual(summary["direct_payload_bytes"], 3096 * 16)
        self.assertEqual(summary["stage_payload_bytes"], 2064 * 16)
        self.assertEqual(summary["algorithm_communication_bytes"], (4608 + 1536) * 16)
        self.assertEqual(summary["latency_us"]["median"], 7)
        self.assertEqual(summary["wait_us"], 0)
        self.assertEqual(summary["measured_traffic_bytes"]["y_out_bytes"], 96 * 1032 * 16 * 5)
        self.assertEqual(summary["submitted_traffic_bytes"]["flag_clear_bytes"], 96 * 6 * 3 * 32 * 16 * 7)
        self.assertEqual(summary["last_launch"], 6)
        self.assertIn("partial_wire=", operators.render_operator_measurements(dict(combine=summary)))
        # Numeric invalidity does not discard complete timing, or turn it into a PASS.
        rows[0]["validation_status"] = "FAIL"
        failed = operators.summarize_operator_measurements(rows, config, "run-test", dict(combine=self.sha))["combine"]
        self.assertEqual(failed["validation_status"], "FAIL")
        self.assertEqual(failed["latency_us"]["median"], 7)
