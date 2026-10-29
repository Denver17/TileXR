"""M5 full multi-launch delivery, module identity and numeric-invalid capture retention."""
import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import shutil
import threading
import time
import unittest
from unittest import mock

import workflow
from current_run_fixture import CurrentRunFixture
from profiling_fixture import combine_loop_events, write_capture


class CurrentCombineTests(CurrentRunFixture):


    def test_normal_contract_keeps_full_validation_without_acceptance_stage(self):
        os.environ.pop("TILEXR_INTERNAL_ACCEPTANCE_STAGE", None)
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = "obsolete"
        self.manifest["checkpoint"] = None
        for operator in ("dispatch", "combine", "both"):
            for mode in ("functional", "performance", "breakdown"):
                with self.subTest(operator=operator, mode=mode):
                    config, mappings, records = self.full_records(operator, mode)
                    self.assertIsNone(config.checkpoint)
                    self.assertEqual(config.install.name, "server-dedup")
                    workflow.validate_rank_records(records, mappings, "run", self.manifest,
                                                   config.chain, check_measurements=False)
                    if operator != "dispatch":
                        records[0]["combine_y_out_validated"] = False
                        with self.assertRaises(workflow.Failure):
                            workflow.validate_rank_records(records, mappings, "run", self.manifest,
                                                           config.chain, check_measurements=False)

    def test_dispatch_and_combined_records_reject_corrupted_execution_evidence(self):
        for operator in ("dispatch", "combine", "both"):
            config, mappings, records = self.full_records(operator)
            mutations = [(None, "pid", 999), (None, "completed", 0),
                         (None, "cleanup", "failed"), (None, "warmup_completed", True),
                         (None, "stream_synchronizations", 2), ("chain", "peer_count", 1)]
            if operator != "dispatch":
                mutations += [(None, "combine_first_magic", 99), (None, "combine_last_magic", 99),
                              (None, "combine_window_offset", 99), (None, "combine_window_mask", 0),
                              (None, "combine_inputs_unchanged", operator == "both")]
            if operator != "combine":
                target = "dispatch" if operator == "both" else None
                mutations += [(target, key, value) for key, value in (
                    ("pid", 999), ("checkpoint", "foreign"), ("last_magic", 99),
                    ("dispatch_window_mask", 0), ("five_tables_validated", False),
                    ("member_addresses_validated", False), ("first_hop_bytes", -1),
                    ("seed", 0), ("bs", 1), ("dtype", "fp32"),
                    ("warmup_completed", 0), ("completed", 0), ("cleanup", "failed"))]
            for nested, key, value in mutations:
                bad = copy.deepcopy(records)
                target = bad[0][nested] if nested else bad[0]
                target[key] = value
                with self.subTest(operator=operator, nested=nested, key=key), self.assertRaises(workflow.Failure):
                    workflow.validate_rank_records(bad, mappings, "run", self.manifest, config.chain)

    def test_topology_accepts_32_hosts_and_rejects_33(self):
        hosts = [dict(host=f"192.0.2.{n+1}", devices=list(range(8))) for n in range(32)]
        self.assertEqual(len(workflow.rank_mapping(hosts)), 256)
        with self.assertRaises(ValueError):
            workflow.rank_mapping(hosts + [dict(host="192.0.2.33", devices=list(range(8)))])

    def test_one_two_and_many_call_window_contracts(self):
        for operator in ("dispatch", "combine", "both"):
            for loops, warmup in ((1, 0), (2, 0), (5, 2)):
                with self.subTest(operator=operator, loops=loops, warmup=warmup):
                    config, mappings, records = self.full_records(operator, loops=loops, warmup=warmup)
                    workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)
                    if operator != "dispatch":
                        self.assertEqual(records[0]["combine_window_mask"], 1 if operator == "both" else 2 if loops == 1 else 3)

    def test_real_m5_contract_and_invalid_identity(self):
        for operator in ("dispatch", "combine", "both"):
            config, mappings, records = self.full_records(operator)
            workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)
            if operator == "dispatch":
                continue
            for key, value in (("implemented_through", "M2"), ("scope", "partial"), ("combine_checked_window_mask", 0), ("combine_result_flags_cleared", False), ("combine_y_out_validated", False),
                    ("combine_final_read_bytes", 0), ("combine_y_out_bytes", 0), ("combine_flag_clear_bytes", 0),
                    ("combine_result_validated", False), ("combine_unwritten_results_unchanged", False),
                    ("combine_direct_wire_bytes", 0), ("combine_direct_payload_bytes", 0),
                    ("combine_partial_validated", False), ("combine_partial_wire_bytes", 0),
                    ("combine_partial_stage_read_bytes", 0), ("combine_partial_stage_read_bytes", 1), ("combine_partial_received_rows", -1),
                    ("combine_type2_rows", 2), ("combine_numeric_rtol", 0.1), ("combine_numeric_policy", "tolerance"),
                    ("combine_delay_us", 50), ("combine_type1_rows", 7), ("combine_observed_rounds", 0)):
                bad = copy.deepcopy(records)
                bad[0][key] = value
                with self.subTest(key=key), self.assertRaises(workflow.Failure):
                    workflow.validate_rank_records(bad, mappings, "run", self.manifest, config.chain)
            invalid = copy.deepcopy(records)
            invalid[0].update(status="FAIL", execution_completed=True, validation_status="FAIL",
                validation_stage="combine-read", validation_error="DIRECT payload mismatch", combine_observed_rounds=0,
                combine_result_validated=False, combine_result_flags_cleared=False)
            workflow.validate_rank_records(invalid, mappings, "run", self.manifest, config.chain, allow_invalid_output=True)
            with self.assertRaises(workflow.Failure):
                workflow.validate_rank_records(invalid, mappings, "run", self.manifest, config.chain)
            invalid[0]["completed"] = 0
            with self.assertRaises(workflow.Failure):
                workflow.validate_rank_records(invalid, mappings, "run", self.manifest, config.chain, allow_invalid_output=True)

    def test_nondedup_loop_capture_uses_world_size_at_node_entry(self):
        # Exercise both decoding and final evidence validation through run_ranks.
        # Only the device subprocess is replaced; parsers and validators are real.
        os.environ.pop("TILEXR_INTERNAL_ACCEPTANCE_STAGE", None)
        self.manifest.update(checkpoint=None, server_dedup_enabled=False)
        for world, rank in ((16, 0), (64, 23)):
            with self.subTest(world=world, rank=rank):
                config, _, records = self.full_records("combine", "breakdown")
                config.chain.update(server_dedup_enabled=False, moe_expert_num=128)
                self.assertNotIn("rank_size", config.chain)
                before = copy.deepcopy(config.chain)
                mapping = workflow.rank_mapping([dict(host=f"192.0.2.{i + 1}", devices=list(range(8)))
                                                 for i in range(world // 8)])[rank]
                row = copy.deepcopy(records[0])
                for marker in (row, row["chain"]):
                    marker.update(workflow.rank_record(mapping, "run", self.manifest),
                                  server_dedup_enabled=False, moe_expert_num=128, peer_count=world)
                bs, h, topk = (config.chain[key] for key in ("bs", "h", "topk"))
                rows, blocks = 10, (2 * h + 479) // 480
                row.update(combine_type1_rows=rows, combine_type2_rows=0, combine_type3_rows=0,
                    combine_token_num=rows, combine_direct_received_rows=bs * topk, combine_partial_received_rows=0,
                    combine_partial_member_rows=0, combine_partial_local_members=0,
                    combine_partial_stage_read_bytes=0, combine_partial_self_read_bytes=0,
                    combine_partial_payload_bytes=0, combine_partial_wire_bytes=0,
                    combine_algorithm_bytes=(rows + bs) * 2 * h, combine_stage_copy_bytes=0,
                    combine_direct_payload_bytes=rows * 2 * h, combine_direct_wire_bytes=rows * blocks * 512,
                    combine_control_publish_bytes=0, combine_final_read_bytes=bs * topk * 2 * h)
                mapping_path = config.install / "share/tilexr/profiling/server_dedup_combine.mapping.json"
                mapping_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(workflow.measurements.profiling_mapping(operator="combine"), mapping_path)

                def execute(argv, log, **kwargs):
                    env = kwargs["env"]
                    path = Path(env["TILEXR_PROFILING_COMBINE_RECORDS_PATH"])
                    write_capture(path, rank=rank, capture_id=env["TILEXR_PROFILING_CAPTURE_ID"],
                        launches=7, warmup=2, events_by_core=combine_loop_events(bs, topk, world, rows))
                    marker = dict(workflow.rank_record(mapping, "run", self.manifest), pid=row["pid"],
                        operator="combine", schema=workflow.measurements.RECORDS_SCHEMA,
                        records_path=str(path), capture_id=env["TILEXR_PROFILING_CAPTURE_ID"], launches=7,
                        capture_mode="latest_launch", captured_launches=1, captured_launch=6)
                    return (workflow.CHAIN_MARKER + json.dumps(row["chain"]) + "\n" +
                            workflow.COMBINE_CHAIN_MARKER + json.dumps(row) + "\n" +
                            workflow.OPERATOR_BREAKDOWN_MARKER + json.dumps(marker) + "\n")

                result, cancelled = dict(errors=[]), threading.Event()
                with mock.patch.object(workflow, "execute", side_effect=execute):
                    workflow.run_ranks(config, "run", self.manifest, [mapping],
                                       time.monotonic() + 60, cancelled, result)
                self.assertEqual(result["errors"], [])
                self.assertFalse(cancelled.is_set())
                self.assertEqual(result["validation_status"], "PASS")
                self.assertEqual(config.chain, before)
                evidence = result["ranks"][0]["operator_measurements"]["combine"]["breakdown"]
                self.assertEqual(evidence["status"], "complete")
                self.assertEqual(evidence["stages_us"]["m3_server_select"]["sample_count"], 32 * world // 8)
                self.assertEqual(evidence["stages_us"]["m6_loop_input_wait"]["sample_count"], bs * topk)
                self.assertTrue(Path(evidence["summary_path"]).is_file())

    def test_numeric_invalid_keeps_both_captures_without_cancelling_peers(self):
        config, mappings, records = self.full_records("both", "performance")
        config.chain["msprof"] = "msprof"
        def execute(argv, log, **kwargs):
            row = copy.deepcopy(records[0]); chain = row.pop("chain"); dispatch = row.pop("dispatch")
            row.update(status="FAIL", execution_completed=True, validation_status="FAIL", validation_stage="combine-read",
                       validation_error="DIRECT payload mismatch", combine_result_validated=False, combine_observed_rounds=0)
            return (workflow.CHAIN_MARKER + json.dumps(chain) + "\n" + workflow.DISPATCH_MARKER + json.dumps(dispatch) + "\n" +
                    workflow.COMBINE_INVALID_MARKER + json.dumps(row) + "\n")
        result = dict(errors=[]); cancelled = threading.Event()
        with mock.patch.object(workflow, "execute", side_effect=execute), \
                mock.patch.object(workflow.operator_measure, "performance_record", side_effect=lambda *a, **kw: {"operator": kw["operator"]}) as capture, \
                mock.patch.object(workflow.operator_measure, "validate_operator_measurements"):
            workflow.run_ranks(config, "run", self.manifest, mappings[:1], time.monotonic() + 60, cancelled, result)
        self.assertFalse(result["errors"])
        self.assertFalse(cancelled.is_set())
        self.assertEqual(result["validation_status"], "FAIL")
        self.assertEqual({call.kwargs["operator"] for call in capture.call_args_list}, {"dispatch", "combine"})
        self.assertEqual(set(result["ranks"][0]["operator_measurements"]), {"dispatch", "combine"})


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CurrentCombineTests(name) for name in CurrentCombineTests.__dict__ if name.startswith("test_"))
