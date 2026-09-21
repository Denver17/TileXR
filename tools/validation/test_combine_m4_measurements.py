"""M4 module-only trace roles and single-launch logical versus wire traffic."""
import copy
import csv
import json
from pathlib import Path
import tempfile
import unittest

import measurements as measure
import operator_measurements as operators
from test_profiling_fixture import CAPTURE_ID, write_capture


class M4MeasurementTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="tilexr-m4-profile-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.config = dict(run_mode="breakdown", operator_mode="combine", rank_size=16, loops=1, warmup=0,
                           implemented_through="M4", scope="partial", combine_send_core_num=32,
                           bs=96, h=516, topk=6, moe_expert_num=96, dtype="bf16", block_dim="auto")
        self.row = dict(rank=0, combine_type3_rows=2, combine_type1_rows=3, combine_token_num=6,
                        combine_algorithm_bytes=5160, combine_stage_copy_bytes=4128,
                        combine_direct_payload_bytes=3096, combine_direct_wire_bytes=4608,
                        combine_control_publish_bytes=24576)
        self.sha = "c" * 64
        self.row.update(combine_type2_rows=1, combine_partial_payload_bytes=1032,
                        combine_partial_wire_bytes=1536, combine_partial_self_read_bytes=1032,
                        combine_partial_member_rows=2, combine_partial_local_members=1, combine_partial_stage_read_bytes=2064, combine_algorithm_bytes=6192)
        self.mapping = Path(__file__).resolve().parents[2] / "src/ep/profiling/server_dedup_combine.mapping.json"

    def events(self):
        events = {}
        for core in range(64):
            pairs = [0x3003, 0x3017, 0x3019 if core >= 32 else 0x301b, 0x3013]
            if core >= 32:
                pairs.insert(3, 0x301d)
            rows = [(0, 0x3001)]
            for index, event in enumerate(pairs, 1):
                rows.extend(((index * 1000, event), (index * 1000 + 500, event + 1)))
            rows.append((6000, 0x3002))
            events[core] = rows
        return events

    def capture(self, events=None, suffix="base"):
        path = write_capture(self.root / (suffix + ".records.v1"), launches=1, warmup=0,
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
        self.assertIn("single-launch", evidence["performance_scope"])
        for fault in ("wrong-role", "missing-module", "missing-end"):
            events = self.events()
            if fault == "wrong-role":
                events[0], events[32] = events[32], events[0]
            elif fault == "missing-module":
                events[0] = [item for item in events[0] if item[1] not in (0x301b, 0x301c)]
            else:
                events[0].pop()
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                self.validate(self.capture(events, fault))
        for key, value in (("wait_us", 50), ("direct_wire_bytes", 0), ("direct_payload_bytes", 0),
                           ("partial_payload_bytes", 0), ("partial_stage_read_bytes", 0), ("partial_wire_bytes", 0), ("stage_copy_bytes", 0), ("implemented_through", "M2")):
            bad = copy.deepcopy(evidence); bad[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(bad)

    def test_single_launch_statistics_keep_stage_and_direct_wire_separate(self):
        path = self.root / "PROF_0/mindstudio_profiler_output/op_summary.csv"
        path.parent.mkdir(parents=True)
        with path.open("w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(("Op Name", "Task Start Time(us)", "Task Duration(us)"))
            writer.writerow((measure.kernel_name("combine"), 1000, 7))
        config = dict(self.config, run_mode="performance")
        rows = []
        for rank in range(16):
            row = dict(self.row, rank=rank)
            evidence = operators.performance_record(path.parent.parent, 1, 0, operator="combine", run_id="run-test",
                                                    rank=rank, kernel_sha=self.sha, config=config, record=row)
            self.assertEqual(evidence["sample_us"], [7])
            rows.append(dict(row, operator_measurements=dict(combine=dict(performance=evidence))))
        summary = operators.summarize_operator_measurements(rows, config, "run-test", dict(combine=self.sha))["combine"]
        self.assertEqual(summary["direct_wire_bytes"], 4608 * 16)
        self.assertEqual(summary["direct_payload_bytes"], 3096 * 16)
        self.assertEqual(summary["stage_payload_bytes"], 2064 * 16)
        self.assertEqual(summary["algorithm_communication_bytes"], (4608 + 1536) * 16)
        self.assertEqual(summary["latency_us"]["median"], 7)
        self.assertEqual(summary["wait_us"], 0)
        self.assertIn("partial_wire=", operators.render_operator_measurements(dict(combine=summary)))
        # Numeric invalidity does not discard complete timing, or turn it into a PASS.
        rows[0]["validation_status"] = "FAIL"
        failed = operators.summarize_operator_measurements(rows, config, "run-test", dict(combine=self.sha))["combine"]
        self.assertEqual(failed["validation_status"], "FAIL")
        self.assertEqual(failed["latency_us"]["median"], 7)
