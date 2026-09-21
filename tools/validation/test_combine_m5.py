"""M5 full multi-launch delivery, module identity and numeric-invalid capture retention."""
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
import test_combine_multi as c5


class CombineM5Tests(c5.CombineMultiTests):
    def setUp(self):
        super().setUp()
        os.environ.update(TILEXR_INTERNAL_ACCEPTANCE_STAGE=workflow.COMBINE_M5_CHECKPOINT,
                          TILEXR_VALIDATION_LOOPS="5", TILEXR_VALIDATION_WARMUP="2")
        self.manifest.update(checkpoint=workflow.COMBINE_M5_CHECKPOINT, mode="production")
        self.manifest["source"]["content_sha256"] = "a" * 64
        self.manifest["artifacts"] = {f"lib/tilexr/kernels/tilexr_ep_{operator}_memory_server_dedup_kernel.o": char * 64
                                      for operator, char in (("dispatch", "d"), ("combine", "c"))}

    def m5_records(self, operator="both", mode="functional", loops=5, warmup=2):
        config, mappings, records = super().multi_records(operator, loops=loops, warmup=warmup)
        config.chain.update(run_mode=mode, implemented_through="M5", scope="full",
            kernel_scope={"both": "dispatch-combine-m5", "combine": "combine-m5", "dispatch": "dispatch"}[operator])
        if mode == "performance":
            config.chain["msprof"] = "msprof"
        for row in records:
            row.update(config.chain)
            row["chain"].update(config.chain)
            if operator != "dispatch":
                h = config.chain["h"]
                bs, k = config.chain["bs"], config.chain["topk"]
                row.pop("y_out_unchanged", None)
                row.update(combine_delay_us=0, combine_empty=False, native_status="combine-m5",
                    combine_stage_validated=True, combine_control_validated=True, combine_control_reserved_unchanged=True,
                    combine_result_validated=True, combine_result_flags_cleared=True, combine_y_out_validated=True, combine_unwritten_results_unchanged=True,
                    combine_type3_rows=2, combine_type1_rows=3, combine_type2_rows=1, combine_token_num=6, combine_direct_received_rows=bs-1,
                    combine_partial_received_rows=1, combine_partial_validated=True,
                    combine_partial_payload_bytes=2 * h, combine_partial_wire_bytes=((2 * h + 479) // 480) * 512,
                    combine_partial_member_rows=2, combine_partial_local_members=1, combine_partial_stage_read_bytes=4 * h, combine_partial_self_read_bytes=2 * h,
                    combine_numeric_policy="fp32-sequential-rne-bit-exact", combine_numeric_atol=0, combine_numeric_rtol=0,
                    combine_algorithm_bytes=(12 + 2 * bs) * h,
                    combine_final_read_bytes=bs * 2 * h, combine_y_out_bytes=bs * 2 * h,
                    combine_flag_clear_bytes=bs * k * ((2 * h + 479) // 480) * 32, combine_stage_copy_bytes=8 * h,
                    combine_direct_payload_bytes=6 * h, combine_direct_wire_bytes=3 * ((2 * h + 479) // 480) * 512,
                    combine_control_publish_bytes=24576, combine_observed_rounds=1,
                    combine_checked_window_mask=row["combine_window_mask"])
                if operator == "both":
                    row["dispatch"]["run_mode"] = mode
        return config, mappings, records

    def test_full_modes_and_package_contract(self):
        for operator in ("dispatch", "combine", "both"):
            for mode in ("functional", "performance", "breakdown"):
                with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": operator,
                        "TILEXR_VALIDATION_RUN_MODE": mode}):
                    cfg = self.config()
                    self.assertEqual((cfg.chain["loops"], cfg.chain["warmup"]), (5, 2))
                    self.assertEqual(cfg.chain["implemented_through"], "M5")
                    self.assertEqual(cfg.install.name, "validation-combine-p24")
        for key, value in (("LOOPS", "0"), ("COMBINE_QUANT_MODE", "4"),
                           ("COMBINE_OBSERVATION", "per-round"), ("COMBINE_FIXTURE", "sparse")):
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_" + key: value}), self.assertRaises(ValueError):
                self.config()
        selected = workflow.COMBINE_TESTS[workflow.COMBINE_M5_CHECKPOINT]
        self.assertIn(workflow.COMBINE_M5_TEST, selected)
        self.assertNotIn(workflow.COMBINE_DELAY_TEST, selected)
        self.assertNotIn(workflow.COMBINE_M12_TEST, selected)
        cases = workflow.combine_m5_cases()
        self.assertEqual(len(cases), 24)
        self.assertEqual({(case["loops"], case["warmup"]) for case in cases}, {(1, 0), (2, 0), (5, 2)})
        for gate in ("F1", "F2", "Fmulti", "P", "B"):
            self.assertEqual({(case["dtype"], case["operator_mode"]) for case in cases if case["gate"] == gate},
                {(dtype, op) for dtype in ("bf16", "fp16") for op in ("combine", "both")})
        self.assertEqual([case["gate"] for case in cases[-4:]], ["B"] * 4)

    def test_normal_contract_keeps_full_validation_without_acceptance_stage(self):
        os.environ.pop("TILEXR_INTERNAL_ACCEPTANCE_STAGE")
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = "obsolete"
        self.manifest["checkpoint"] = None
        for operator in ("dispatch", "combine", "both"):
            for mode in ("functional", "performance", "breakdown"):
                with self.subTest(operator=operator, mode=mode):
                    config, mappings, records = self.m5_records(operator, mode)
                    self.assertIsNone(config.checkpoint)
                    self.assertEqual(config.install.name, "server-dedup")
                    workflow.validate_rank_records(records, mappings, "run", self.manifest,
                                                   config.chain, check_measurements=False)
                    if operator != "dispatch":
                        records[0]["combine_y_out_validated"] = False
                        with self.assertRaises(workflow.Failure):
                            workflow.validate_rank_records(records, mappings, "run", self.manifest,
                                                           config.chain, check_measurements=False)

    def test_one_two_and_many_call_window_contracts(self):
        for operator in ("dispatch", "combine", "both"):
            for loops, warmup in ((1, 0), (2, 0), (5, 2)):
                with self.subTest(operator=operator, loops=loops, warmup=warmup):
                    config, mappings, records = self.m5_records(operator, loops=loops, warmup=warmup)
                    workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)
                    if operator != "dispatch":
                        self.assertEqual(records[0]["combine_window_mask"], 1 if operator == "both" else 2 if loops == 1 else 3)

    def test_real_m5_contract_and_invalid_identity(self):
        for operator in ("dispatch", "combine", "both"):
            config, mappings, records = self.m5_records(operator)
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

    def test_numeric_invalid_keeps_both_captures_without_cancelling_peers(self):
        config, mappings, records = self.m5_records("both", "performance")
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

    def test_historical_resume_is_rejected_before_build(self):
        config = workflow.Config("commit-validate")
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_RESUME_SUMMARY": "old-p21.json"}), \
                mock.patch.object(workflow, "build") as build, redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            code = workflow.combine_m5_commit_validate(config, "resume-m5")
        self.assertNotEqual(code, 0)
        build.assert_not_called()
        summary = workflow.read_json(config.logs / "resume-m5/summary.json")
        self.assertEqual(summary["cases"], [])
        self.assertIn("historical resume", summary["errors"][0]["message"])

    def test_gate_rejects_build_host_sync_case_rank_and_provenance_failures(self):
        faults = (None, "build-profile-off", "host-profile-off", "sync-profile-off", "build-profile-on",
                  "host-profile-on", "sync-profile-on", "F2-both-fp16", "B-combine-bf16",
                  "wrong-build", "wrong-checkpoint", "missing-rank", "source-change", "exception")
        for fault in faults:
            config = workflow.Config("commit-validate")
            manifest = copy.deepcopy(self.manifest)
            builds, launched = [], []
            def build(cfg):
                package = "profile-on" if cfg.profiling else "profile-off"
                builds.append(package)
                cfg.build_id = package
                if fault == "build-" + package:
                    raise ValueError("injected build")
                manifest.update(build_id=package, server_dedup_profiling=cfg.profiling)
                if fault == "source-change" and cfg.profiling:
                    manifest["source"]["content_sha256"] = "b" * 64
            def host(cfg, run_id):
                return int(fault == "host-" + ("profile-on" if cfg.profiling else "profile-off"))
            def sync(cfg):
                self.assertEqual(len(cfg.hosts()), 2)
                package = "profile-on" if cfg.chain["run_mode"] == "breakdown" else "profile-off"
                if fault == "sync-" + package:
                    raise ValueError("injected sync")
            def run(cfg):
                name = cfg.logs.name
                expected_case = next(case for case in workflow.combine_m5_cases() if case["name"] == name)
                self.assertEqual((cfg.chain["loops"], cfg.chain["warmup"]),
                                 (expected_case["loops"], expected_case["warmup"]))
                launched.append(name)
                if fault == "exception":
                    raise ValueError("injected run")
                cfg.last_summary_path = cfg.logs / "result/summary.json"
                workflow.write_json(cfg.last_summary_path, dict(status="FAIL" if name == fault else "PASS",
                    build_id="wrong" if fault == "wrong-build" else manifest["build_id"],
                    checkpoint="old" if fault == "wrong-checkpoint" else config.checkpoint,
                    rank_size=16, completed_ranks=15 if fault == "missing-rank" else 16))
                return int(name == fault)
            original = dict(os.environ)
            output = io.StringIO()
            with self.subTest(fault=fault), mock.patch.object(workflow, "build", side_effect=build), \
                    mock.patch.object(workflow, "verify_build", side_effect=lambda *a, **kw: copy.deepcopy(manifest)), \
                    mock.patch.object(workflow, "host_validate", side_effect=host), mock.patch.object(workflow, "sync", side_effect=sync), \
                    mock.patch.object(workflow, "run", side_effect=run), redirect_stdout(output), redirect_stderr(io.StringIO()):
                code = workflow.combine_m5_commit_validate(config, "gate-" + str(fault))
            self.assertEqual(os.environ, original)
            summary = workflow.read_json(config.logs / ("gate-" + str(fault)) / "summary.json")
            self.assertEqual(code == 0, fault is None)
            names = [row["name"] for row in workflow.combine_m5_cases()]
            self.assertEqual(launched, names[:len(launched)])
            self.assertEqual(summary["expected_cases"], 24)
            self.assertEqual(summary["scope"], "full")
            if fault is None:
                self.assertEqual(builds, ["profile-off", "profile-on"])
                self.assertEqual(len(summary["cases"]), 24)
                self.assertTrue(all(gate["status"] == "PASS" for gate in summary["gates"].values()))
                self.assertEqual(summary["phases"][0]["install_directory"], summary["phases"][1]["install_directory"])
                self.assertEqual(summary["phases"][0]["build_directory"], summary["phases"][1]["build_directory"])
                for index, name in enumerate(names, 1):
                    self.assertIn(f"[{index}/24] PASS {name}", output.getvalue())
            else:
                self.assertNotEqual(summary["gates"]["B"]["status"], "PASS")


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CombineM5Tests(name) for name in CombineM5Tests.__dict__ if name.startswith("test_"))
