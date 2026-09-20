#!/usr/bin/env python3
"""C5 controller contracts: continuous batches, independent magic and bounded matrix."""

import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
import threading
import time
import unittest
from unittest import mock

import workflow
import test_combine_chain as c4


class CombineMultiTests(c4.CombineChainTests):
    def setUp(self):
        super().setUp()
        os.environ.update(TILEXR_VALIDATION_CHECKPOINT=workflow.COMBINE_MULTI_CHECKPOINT,
                          TILEXR_VALIDATION_LOOPS="5", TILEXR_VALIDATION_WARMUP="2")
        self.manifest["checkpoint"] = workflow.COMBINE_MULTI_CHECKPOINT

    def multi_records(self, operator, loops=5, warmup=2, quant=0):
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": operator,
                "TILEXR_VALIDATION_LOOPS": str(loops), "TILEXR_VALIDATION_WARMUP": str(warmup)}):
            config, mappings, records = super().records(operator != "combine")
        config.chain.update(operator_mode=operator, dispatch_quant_mode=quant, quant_mode=quant,
                            kernel_scope="dispatch" if operator == "dispatch" else
                            "dispatch-combine-empty" if operator == "both" else "combine-empty")
        total = loops + warmup
        both = operator == "both"
        for record in records:
            record.update(config.chain)
            record["chain"].update(config.chain)
            record.update(submitted=loops, completed=loops, warmup_submitted=warmup,
                          warmup_completed=warmup, stream_synchronizations=1)
            if operator != "combine":
                dispatch = record["dispatch"]
                dispatch.update(config.chain)
                dispatch.update(kernel_scope="dispatch", submitted=loops, completed=loops,
                    warmup_submitted=warmup, warmup_completed=warmup, stream_synchronizations=1,
                    dispatch_first_magic=1, dispatch_window_mask=2 if both or total == 1 else 3,
                    last_magic=2 * total - 1 if both else total,
                    window_offset=workflow.MEMORY_BOUNDARY_BYTES if both else total % 2 * workflow.MEMORY_BOUNDARY_BYTES)
                if quant:
                    old_stride = dispatch["token_stride"]
                    stride = workflow.source_send_stride(config.chain)
                    for name in ("first_hop_bytes", "direct_bytes", "relay_bytes", "data_bytes",
                                 "local_data_bytes", "same_server_data_bytes", "cross_server_data_bytes"):
                        dispatch[name] = dispatch[name] // old_stride * stride
                    dispatch["token_stride"] = stride
            for name in ("dispatch", "combine"):
                active = operator in (name, "both")
                first = (2 if name == "combine" and both else 1) if active else 0
                last = (2 * total - int(name == "dispatch") if both else total) if active else 0
                record.update({name + "_first_magic": first, name + "_last_magic": last,
                               name + "_window_mask": (1 << (first % 2) if both or total == 1 else 3) if active else 0,
                               name + "_window_offset": last % 2 * workflow.MEMORY_BOUNDARY_BYTES})
                for stage, count in (("", loops), ("warmup_", warmup)):
                    for state in ("submitted", "completed"):
                        record[name + "_" + stage + state] = count if active else 0
            record.update(combine_input_check="dispatch-final-reference" if both else "full-snapshot",
                          combine_inputs_unchanged=not both, independent_expert_out_unchanged=not both or quant == 4,
                          expert_out_source=("separate_expert_out" if quant else "dispatch_expand_x") if both
                          else "standalone_expert_out")
        if operator == "dispatch":
            records = [dict(record["dispatch"], chain=record["chain"]) for record in records]
        return config, mappings, records

    def test_functional_batch_configuration_and_isolated_package(self):
        for operator in ("dispatch", "combine", "both"):
            for loops, warmup in ((1, 0), (2, 1), (5, 2), (2 ** 32 - 1, 2 ** 32 - 1)):
                with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": operator,
                        "TILEXR_VALIDATION_LOOPS": str(loops), "TILEXR_VALIDATION_WARMUP": str(warmup)}):
                    config = self.config()
                    self.assertEqual(config.install.name, "validation-combine-c5")
                    self.assertEqual((config.chain["loops"], config.chain["warmup"]), (loops, warmup))
        for key, value in (("RUN_MODE", "performance"), ("RUN_MODE", "breakdown"), ("BUILD_MODE", "host"),
                           ("LOOPS", "0"), ("WARMUP", "-1"), ("COMBINE_QUANT_MODE", "4")):
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_" + key: value}), self.assertRaises(ValueError):
                self.config()
        config = workflow.Config("commit-validate")
        self.assertEqual(config.hostfile, self.hostfile)
        self.assertEqual(workflow.COMBINE_TESTS[config.checkpoint],
                         workflow.COMBINE_TESTS[workflow.COMBINE_CHAIN_CHECKPOINT] | {workflow.COMBINE_MULTI_TEST})

    def test_rank_contract_rejects_bad_counts_windows_and_snapshot_overclaims(self):
        for operator in ("dispatch", "combine", "both"):
            for loops, warmup in ((1, 0), (2, 0), (5, 2)):
                config, mappings, records = self.multi_records(operator, loops, warmup)
                workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)
                mutations = [(None, "stream_synchronizations", 2), (None, "completed", loops + warmup + 1),
                             (None, "warmup_completed", True), (None, "cleanup", "failed")]
                if operator == "dispatch":
                    mutations += [(None, "last_magic", 99), (None, "window_offset", 99),
                                  (None, "dispatch_window_mask", 0), (None, "dispatch_first_magic", 2)]
                else:
                    mutations += [(None, "combine_last_magic", loops), (None, "combine_window_mask", 0),
                                  (None, "dispatch_window_mask", 3), (None, "combine_warmup_completed", -1),
                                  (None, "dispatch_warmup_submitted", -1), (None, "combine_submitted", 0),
                                  (None, "combine_window_offset", 99), (None, "combine_first_magic", 99),
                                  (None, "combine_delay_us", 0), (None, "combine_input_check", "unverified"),
                                  (None, "combine_inputs_unchanged", operator == "both"),
                                  (None, "independent_expert_out_unchanged", operator == "both"),
                                  (None, "five_tables_linked", operator != "both")]
                if operator == "both":
                    mutations += [("dispatch", "last_magic", loops + warmup),
                                  ("dispatch", "dispatch_window_mask", 3),
                                  ("dispatch", "stream_synchronizations", 2),
                                  ("dispatch", "five_tables_validated", False)]
                for nested, key, value in mutations:
                    changed = copy.deepcopy(records)
                    target = changed[0][nested] if nested else changed[0]
                    if type(target.get(key)) is type(value) and target[key] == value:
                        continue
                    target[key] = value
                    with self.subTest(operator=operator, loops=loops, warmup=warmup, key=key, nested=nested), \
                            self.assertRaises(workflow.Failure):
                        workflow.validate_rank_records(changed, mappings, "run", self.manifest, config.chain)
        config, mappings, records = self.multi_records("both", quant=4)
        workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)
        records[0]["independent_expert_out_unchanged"] = False
        with self.assertRaises(workflow.Failure):
            workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)

    def test_rank_runner_parses_all_three_modes_and_requires_each_marker(self):
        for operator in ("dispatch", "combine", "both"):
            config, mappings, records = self.multi_records(operator)
            for fault in (None, "missing", "duplicate", "invalid-dispatch"):
                def execute(argv, log, **kwargs):
                    rank = int(argv[-2])
                    self.assertEqual(kwargs["env"]["TILEXR_VALIDATION_LOOPS"], "5")
                    self.assertEqual(kwargs["env"]["TILEXR_VALIDATION_WARMUP"], "2")
                    record = dict(records[rank])
                    chain = record.pop("chain")
                    dispatch = record.pop("dispatch", None)
                    output = workflow.CHAIN_MARKER + json.dumps(chain) + "\n"
                    if dispatch is not None:
                        output += workflow.DISPATCH_MARKER + json.dumps(dispatch) + "\n"
                    marker = workflow.DISPATCH_MARKER if operator == "dispatch" else workflow.COMBINE_CHAIN_MARKER
                    if fault != "missing":
                        output += marker + json.dumps(record) + "\n"
                    if fault == "duplicate":
                        output += marker + json.dumps(record) + "\n"
                    if fault == "invalid-dispatch":
                        output += workflow.DISPATCH_INVALID_MARKER + "{}\n"
                    return output
                result = dict(errors=[])
                with mock.patch.object(workflow, "execute", side_effect=execute):
                    workflow.run_ranks(config, "run", self.manifest, mappings, time.monotonic() + 60,
                                       threading.Event(), result)
                self.assertEqual(bool(result["errors"]), fault is not None, (operator, fault, result))

    def test_eight_case_matrix_progress_fail_fast_and_result_paths(self):
        for failing_case in (None, 1, 5, 8):
            config = workflow.Config("commit-validate")
            seen = []
            def run_case(case):
                seen.append(dict(case.chain))
                case.last_summary_path = case.logs / "case-run/summary.json"
                passed = len(seen) != failing_case
                workflow.write_json(case.last_summary_path, dict(status="PASS" if passed else "FAIL",
                                     rank_size=16, completed_ranks=16 if passed else 15))
                return 0 if passed else 1
            output = io.StringIO()
            before = dict(os.environ)
            with mock.patch.object(workflow, "verify_build", return_value=self.manifest), \
                    mock.patch.object(workflow, "host_validate", return_value=0), \
                    mock.patch.object(workflow, "sync") as sync, \
                    mock.patch.object(workflow, "run", side_effect=run_case), \
                    redirect_stdout(output), redirect_stderr(io.StringIO()):
                code = workflow.combine_chain_commit_validate(config, "multi-" + str(failing_case))
            self.assertEqual(os.environ, before)
            self.assertEqual(code == 0, failing_case is None)
            self.assertEqual(len(seen), failing_case or 8)
            sync.assert_called_once()
            self.assertEqual(len(sync.call_args.args[0].hosts()), 2)
            report = workflow.read_json(config.logs / ("multi-" + str(failing_case)) / "summary.json")
            self.assertEqual(report["expected_cases"], 8)
            self.assertEqual(len({case["summary"] for case in report["cases"]}), len(seen))
            for index, case in enumerate(seen, 1):
                self.assertEqual((case["loops"], case["warmup"]), (5, 2))
                self.assertIn(f"[{index}/8] START", output.getvalue())
                self.assertIn(f"[{index}/8] {'FAIL' if index == failing_case else 'PASS'}", output.getvalue())
            self.assertEqual([(case["operator_mode"], case["dtype"], case["dispatch_quant_mode"], case["fp8_format"])
                              for case in seen], [case[1:] for case in workflow.COMBINE_MULTI_CASES[:len(seen)]])

    def test_host_multi_driver_requires_unique_current_marker(self):
        config = workflow.Config("host-validate")
        test = dict(name=workflow.COMBINE_MULTI_TEST, argv=["@python", "tests/ep/integration/test_server_dedup_combine_multi.py",
                    "--consumer", "install/validation-combine-c5/bin/" + workflow.COMBINE_DRY_RUN_PROGRAM])
        marker = "EP_SERVER_DEDUP_COMBINE_MULTI_PASS test=lifecycle\n"
        for output in (marker, "", marker * 2, "EP_SERVER_DEDUP_COMBINE_CHAIN_PASS test=lifecycle\n"):
            with mock.patch.object(workflow, "execute", return_value=output):
                if output == marker:
                    workflow.run_validation_test(config, test, config.logs, 120)
                else:
                    with self.assertRaises(workflow.Failure):
                        workflow.run_validation_test(config, test, config.logs, 120)


def load_tests(loader, tests, pattern):
    # C4 owns its inherited tests; this module only runs the new C5 cases.
    return unittest.TestSuite(CombineMultiTests(name) for name in CombineMultiTests.__dict__ if name.startswith("test_"))


if __name__ == "__main__":
    unittest.main()
