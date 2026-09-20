#!/usr/bin/env python3
"""C4 gate, rank evidence and independent operator configuration behavior."""

import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import threading
import time
import os
import unittest
from types import SimpleNamespace
from unittest import mock

import workflow
from test_workflow import TemporaryRoot


class CombineChainTests(TemporaryRoot):
    def setUp(self):
        super().setUp()
        self.hostfile.write_text("192.0.2.1\n192.0.2.2\n192.0.2.3\n")
        os.environ.update(TILEXR_VALIDATION_CHECKPOINT=workflow.COMBINE_CHAIN_CHECKPOINT,
                          TILEXR_VALIDATION_BUILD_MODE="production", TILEXR_VALIDATION_OPERATOR_MODE="both",
                          TILEXR_VALIDATION_SERVERS="2", TILEXR_COMM_ID="192.0.2.1:10177")
        os.environ.pop("TILEXR_DEMO_BARRIER_ADDR", None)
        self.manifest = dict(checkpoint=workflow.COMBINE_CHAIN_CHECKPOINT, build_id="build",
                             source=dict(commit="a" * 40, provenance="git"))

    def test_only_single_functional_production_modes_are_accepted(self):
        for operator in ("dispatch", "combine", "both"):
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": operator}):
                config = self.config()
                self.assertEqual(len(config.hosts()), 2)
                self.assertEqual(config.install.name, "validation-combine-c4")
                self.assertEqual((config.chain["loops"], config.chain["warmup"]), (1, 0))
        for key, value in (("RUN_MODE", "breakdown"), ("RUN_MODE", "performance"), ("LOOPS", "2"),
                           ("WARMUP", "1"), ("BUILD_MODE", "host"), ("COMBINE_QUANT_MODE", "4")):
            with self.subTest(key=key, value=value), mock.patch.dict(os.environ, {
                    "TILEXR_VALIDATION_" + key: value}), self.assertRaises(ValueError):
                self.config()
        custom = self.root / "deployment.hosts"
        custom.write_text(self.hostfile.read_text())
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_HOSTFILE": str(custom),
                                         "TILEXR_VALIDATION_SSH_USER": "test-user"}):
            config = workflow.Config("commit-validate")
            self.assertEqual(config.hostfile, custom)
            self.assertEqual(config.user, "test-user")

    def records(self, both=True):
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": "both" if both else "combine"}):
            config = self.config()
        mappings = workflow.rank_mapping(config.hosts())[:8]
        records = []
        for mapping in mappings:
            identity = dict(workflow.rank_record(mapping, "run", self.manifest), pid=100 + mapping["rank"],
                            server_dedup_enabled=True, **config.chain)
            record = dict(identity, chain=dict(identity, peer_count=16, peer_mem_bytes=8 * 1024 ** 3),
                submitted=1, completed=1, warmup_submitted=0, warmup_completed=0, cleanup="complete",
                dispatch_submitted=int(both), dispatch_completed=int(both), combine_submitted=1, combine_completed=1,
                dispatch_last_magic=int(both), combine_last_magic=2 if both else 1,
                dispatch_window_offset=4 * 1024 ** 3 if both else 0,
                combine_window_offset=0 if both else 4 * 1024 ** 3, combine_delay_us=50,
                combine_empty=True, combine_inputs_unchanged=True, y_out_unchanged=True,
                five_tables_linked=both, send_counts_linked=both,
                expert_out_source="dispatch_expand_x" if both else "standalone_expert_out")
            if both:
                record["dispatch"] = dict(identity, kernel_scope="dispatch", native_status="dispatch",
                    submitted=1, completed=1, warmup_submitted=0, warmup_completed=0, cleanup="complete",
                    first_hop_packets=2, first_hop_received=2, first_hop_bytes=31744, token_stride=15872,
                    direct_packets=3, direct_received=3, direct_bytes=47616,
                    local_data_bytes=15872, same_server_data_bytes=47616, cross_server_data_bytes=47616,
                    relay_packets=2, relay_received=2, relay_bytes=31744, data_bytes=111104,
                    relay_validated=True, dispatch_outputs_validated=True, five_tables_validated=True,
                    member_addresses_validated=True, flags_cleared=True, first_hop_validated=True,
                    direct_validated=True, route_coverage_validated=True, source_outputs_validated=True,
                    weights_unchanged=True)
            records.append(record)
        return config, mappings, records

    def test_exact_magic_windows_pointer_links_and_retained_dispatch_checks(self):
        for both in (False, True):
            config, mappings, records = self.records(both)
            workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)
            corruptions = [(None, "combine_last_magic", 99), (None, "combine_window_offset", 999),
                           (None, "dispatch_window_offset", 999), (None, "combine_submitted", 0),
                           (None, "combine_completed", True), (None, "combine_delay_us", 0),
                           (None, "combine_inputs_unchanged", False), (None, "y_out_unchanged", False),
                           (None, "five_tables_linked", not both), (None, "send_counts_linked", not both),
                           (None, "expert_out_source", "fp8_dispatch_expand_x"),
                           (None, "dispatch_quant_mode", 4), ("chain", "combine_quant_mode", 4)]
            if both:
                corruptions += [("dispatch", "five_tables_validated", False),
                                ("dispatch", "pid", 999), ("dispatch", "checkpoint", "wrong")]
            for nested, key, value in corruptions:
                changed = copy.deepcopy(records)
                (changed[0][nested] if nested else changed[0])[key] = value
                with self.subTest(both=both, nested=nested, key=key), self.assertRaises(workflow.Failure):
                    workflow.validate_rank_records(changed, mappings, "run", self.manifest, config.chain)
            with self.assertRaises(workflow.Failure):
                workflow.validate_rank_records(records[:-1], mappings, "run", self.manifest, config.chain)

    def test_rank_runner_requires_both_markers_and_propagates_operator_configuration(self):
        for both, fault in ((False, None), (True, None), (True, "missing-dispatch"),
                            (False, "unexpected-dispatch"), (True, "duplicate-combine")):
            config, mappings, records = self.records(both)
            seen = []

            def execute(argv, log, **kwargs):
                rank = int(argv[-2])
                seen.append(rank)
                env = kwargs["env"]
                self.assertEqual(env["TILEXR_VALIDATION_OPERATOR_MODE"], "both" if both else "combine")
                self.assertEqual(env["TILEXR_VALIDATION_COMBINE_QUANT_MODE"], "0")
                self.assertNotIn("TILEXR_PROFILING_RECORDS_PATH", env)
                record = dict(records[rank])
                chain = record.pop("chain")
                dispatch = record.pop("dispatch", {})
                output = workflow.CHAIN_MARKER + json.dumps(chain) + "\n"
                if (both and fault != "missing-dispatch") or fault == "unexpected-dispatch":
                    output += workflow.DISPATCH_MARKER + json.dumps(dispatch) + "\n"
                output += workflow.COMBINE_CHAIN_MARKER + json.dumps(record) + "\n"
                if fault == "duplicate-combine":
                    output += workflow.COMBINE_CHAIN_MARKER + json.dumps(record) + "\n"
                return output

            result = dict(errors=[])
            with self.subTest(both=both, fault=fault), mock.patch.object(workflow, "execute", side_effect=execute):
                workflow.run_ranks(config, "run", self.manifest, mappings, time.monotonic() + 60,
                                   threading.Event(), result)
                self.assertEqual(bool(result["errors"]), fault is not None)
                if fault is None:
                    self.assertEqual(len(result["ranks"]), 8)
                    self.assertEqual(sorted(seen), list(range(8)))

    def test_c4_package_dispatch_only_parser_preserves_checkpoint_and_full_validation(self):
        _, mappings, combined = self.records()
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": "dispatch"}):
            config = self.config()
        self.assertEqual(config.chain["kernel_scope"], "dispatch")
        for fault in (None, "missing-dispatch", "unexpected-combine", "five-tables", "wrong-checkpoint"):
            def execute(argv, log, **kwargs):
                rank = int(argv[-2])
                self.assertEqual(kwargs["env"]["TILEXR_VALIDATION_OPERATOR_MODE"], "dispatch")
                original = combined[rank]
                chain = dict(original["chain"], **config.chain)
                dispatch = dict(original["dispatch"], **config.chain)
                if fault == "five-tables":
                    dispatch["five_tables_validated"] = False
                elif fault == "wrong-checkpoint":
                    dispatch["checkpoint"] = workflow.DISPATCH_CHECKPOINT
                output = workflow.CHAIN_MARKER + json.dumps(chain) + "\n"
                if fault != "missing-dispatch":
                    output += workflow.DISPATCH_MARKER + json.dumps(dispatch) + "\n"
                if fault == "unexpected-combine":
                    output += workflow.COMBINE_CHAIN_MARKER + json.dumps(original) + "\n"
                return output

            result = dict(errors=[])
            with self.subTest(fault=fault), mock.patch.object(workflow, "execute", side_effect=execute):
                workflow.run_ranks(config, "run", self.manifest, mappings, time.monotonic() + 60,
                                   threading.Event(), result)
                self.assertEqual(bool(result["errors"]), fault is not None)
                if fault is None:
                    self.assertEqual(len(result["ranks"]), 8)
                    self.assertTrue(all(record["checkpoint"] == workflow.COMBINE_CHAIN_CHECKPOINT and
                                        record["chain"]["checkpoint"] == workflow.COMBINE_CHAIN_CHECKPOINT and
                                        record["kernel_scope"] == "dispatch" and "dispatch" not in record
                                        for record in result["ranks"]))

    def test_controller_checks_dispatch_tables_for_c4_dispatch_and_both(self):
        for operator in ("dispatch", "both"):
            for invalid_tables in (False, True):
                with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": operator}):
                    config = self.config()
                ranks = [dict(rank=rank, checkpoint=config.checkpoint) for rank in range(16)]
                nodes = []
                for index in range(2):
                    records = ranks[index * 8:(index + 1) * 8]
                    nodes.append(dict(status="PASS", errors=[], tests=[], ranks=
                                      [dict(rank=item["rank"], dispatch=item) for item in records]
                                      if operator == "both" else records))
                    path = config.logs / "run" / ("nodes/1/result.json" if index else "result.json")
                    workflow.write_json(path, nodes[-1])

                def launch(log, error, run_id, manifest, expected, chain):
                    return nodes[expected[0]["host_index"]], []

                with self.subTest(operator=operator, invalid_tables=invalid_tables), \
                        mock.patch.object(workflow, "verify_build", return_value=self.manifest), \
                        mock.patch.object(workflow, "check_coordinator"), mock.patch.object(workflow, "execute"), \
                        mock.patch.object(workflow, "launch_outcome", side_effect=launch), \
                        mock.patch.object(workflow, "result_errors", return_value=[]), \
                        mock.patch.object(workflow.uuid, "uuid4", return_value=SimpleNamespace(hex="run")), \
                        mock.patch.object(workflow, "validate_dispatch_tables", side_effect=
                                          ValueError("bad tables") if invalid_tables else None) as tables, \
                        redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    self.assertEqual(workflow.run(config) == 0, not invalid_tables)
                checked, shape = tables.call_args.args
                self.assertEqual(sorted(item["rank"] for item in checked), list(range(16)))
                self.assertEqual(shape["rank_size"], 16)
                report = workflow.read_json(config.last_summary_path)
                self.assertEqual(report["status"], "FAIL" if invalid_tables else "PASS")
                self.assertEqual(report["checkpoint"], workflow.COMBINE_CHAIN_CHECKPOINT)
                self.assertEqual(report.get("dispatch_five_tables_validated", False), not invalid_tables)

    def test_six_case_gate_uses_normal_sync_run_and_stops_on_failure(self):
        for failure in (None, "host", "sync", "rank", "case", "launch"):
            with self.subTest(failure=failure):
                config = workflow.Config("commit-validate")
                seen = []

                def run_case(case):
                    seen.append(dict(case.chain))
                    if failure == "launch":
                        raise workflow.Failure("LAUNCH_FAILED", "injected SSH launch failure")
                    case.last_summary_path = case.logs / "case-run/summary.json"
                    workflow.write_json(case.last_summary_path, dict(status="FAIL" if failure == "case" else "PASS",
                        rank_size=16, completed_ranks=15 if failure == "rank" else 16))
                    return 1 if failure == "case" else 0

                def sync_case(case):
                    self.assertEqual(len(case.hosts()), 2)
                    if failure == "sync":
                        raise workflow.Failure("SYNC_FAILED", "injected transport failure")

                before = dict(os.environ)
                with mock.patch.object(workflow, "verify_build", return_value=self.manifest), \
                        mock.patch.object(workflow, "host_validate", return_value=1 if failure == "host" else 0), \
                        mock.patch.object(workflow, "sync", side_effect=sync_case) as sync, \
                        mock.patch.object(workflow, "run", side_effect=run_case), \
                        redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    code = workflow.combine_chain_commit_validate(config, "commit-" + str(failure))
                self.assertEqual(os.environ, before)
                report = workflow.read_json(config.logs / ("commit-" + str(failure)) / "summary.json")
                self.assertEqual(code == 0, failure is None)
                self.assertEqual(report["status"], "PASS" if failure is None else "FAIL")
                if failure == "host":
                    sync.assert_not_called()
                self.assertEqual(len(seen), 6 if failure is None else 0 if failure in ("host", "sync") else 1)
                if failure == "launch":
                    retained = workflow.read_json(report["cases"][0]["summary"])
                    self.assertEqual(retained["errors"][0]["type"], "LAUNCH_FAILED")
                if failure is None:
                    self.assertEqual(len({item["summary"] for item in report["cases"]}), 6)
                    self.assertEqual([(case["operator_mode"], case["dtype"], case["dispatch_quant_mode"], case["fp8_format"])
                                      for case in seen], [item[1:] for item in workflow.COMBINE_CHAIN_CASES])
                    self.assertTrue(all(case["combine_quant_mode"] == 0 and case["loops"] == 1 and case["warmup"] == 0
                                        for case in seen))

    def test_missing_hostfile_is_failure_before_sync_or_launch(self):
        self.hostfile.unlink()
        config = workflow.Config("commit-validate")
        with mock.patch.object(workflow, "verify_build", return_value=self.manifest), \
                mock.patch.object(workflow, "host_validate", return_value=0), \
                mock.patch.object(workflow, "sync") as sync, mock.patch.object(workflow, "run") as run, \
                redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertNotEqual(workflow.combine_chain_commit_validate(config, "missing-hosts"), 0)
        sync.assert_not_called()
        run.assert_not_called()
        report = workflow.read_json(config.logs / "missing-hosts/summary.json")
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("hostfile", report["errors"][0]["message"])

    def test_remote_cancel_preserves_case_log_root(self):
        config = self.config()
        config.logs = self.root / "run/validation/commit/cases/both"
        argv = workflow.remote_argv(config, "192.0.2.2", "cancel", "run")
        self.assertIn("TILEXR_VALIDATION_RESULT_ROOT", argv[-1])
        self.assertIn(str(config.logs), argv[-1])
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_RESULT_ROOT": str(config.logs)}), \
                mock.patch.object(workflow.sys, "argv", ["workflow.py", "cancel", "run"]), \
                mock.patch.object(workflow, "cancel") as cancel:
            self.assertEqual(workflow.main(), 0)
        self.assertEqual(cancel.call_args.args[0].logs, config.logs)


if __name__ == "__main__":
    unittest.main()
