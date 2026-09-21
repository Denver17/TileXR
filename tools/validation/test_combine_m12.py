#!/usr/bin/env python3
"""P2.1 gate completeness, package isolation and module evidence failures."""
import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import os
from pathlib import Path
import unittest
from unittest import mock

import workflow
import test_combine_multi as c5


class CombineM12Tests(c5.CombineMultiTests):
    def setUp(self):
        super().setUp()
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.COMBINE_M12_CHECKPOINT
        self.manifest.update(checkpoint=workflow.COMBINE_M12_CHECKPOINT, mode="production")
        self.manifest["source"]["content_sha256"] = "a" * 64
        self.manifest["artifacts"] = {f"lib/tilexr/kernels/tilexr_ep_{operator}_memory_server_dedup_kernel.o": char * 64
                                      for operator, char in (("dispatch", "d"), ("combine", "c"))}

    def m12_records(self, operator="both", observation="batch"):
        config, mappings, records = super().multi_records(operator)
        config.chain.update(kernel_scope={"both": "dispatch-combine-m12", "combine": "combine-m12", "dispatch": "dispatch"}[operator],
                            combine_observation=observation)
        for row in records:
            row.update(config.chain)
            row["chain"].update(config.chain)
            row["stream_synchronizations"] = 7 if observation == "per-round" else 1
            if operator != "dispatch":
                row.update(combine_empty=False, native_status="combine-m12", combine_stage_validated=True,
                           combine_control_validated=True, combine_control_reserved_unchanged=True,
                           combine_result_unchanged=True, combine_type3_rows=2, combine_token_num=4,
                           combine_algorithm_bytes=4 * config.chain["h"], combine_stage_copy_bytes=8 * config.chain["h"],
                           combine_control_publish_bytes=24576, combine_observed_rounds=7 if observation == "per-round" else 1,
                           combine_checked_window_mask=row["combine_window_mask"])
                if operator == "both":
                    row["dispatch"]["stream_synchronizations"] = row["stream_synchronizations"]
        return config, mappings, records

    def test_module_configuration_and_separate_install_paths(self):
        for mode in ("functional", "performance", "breakdown"):
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_RUN_MODE": mode}):
                config = self.config()
                self.assertEqual(config.chain["implemented_through"], "M2")
                self.assertEqual(config.chain["scope"], "partial")
                self.assertEqual(config.install.name, "validation-combine-p21-profile-" + ("on" if mode == "breakdown" else "off"))
                command = workflow.remote_argv(config, "192.0.2.2", "node", "run", "build", "1")[-1]
                self.assertIn("TILEXR_VALIDATION_COMBINE_FIXTURE=balanced", command)
                self.assertIn("TILEXR_VALIDATION_BREAKDOWN=" + str(int(mode == "breakdown")), command)
        for key, value in (("COMBINE_QUANT_MODE", "4"), ("COMBINE_FIXTURE", "unknown"),
                           ("COMBINE_FIXTURE", "sparse"), ("COMBINE_OBSERVATION", "sometimes")):
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_" + key: value}), self.assertRaises(ValueError):
                self.config()
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_RUN_MODE": "performance",
                                         "TILEXR_VALIDATION_COMBINE_OBSERVATION": "per-round"}), self.assertRaises(ValueError):
            self.config()

    def test_rank_evidence_requires_real_stage_control_protection_and_observation(self):
        for operator in ("dispatch", "combine", "both"):
            for observation in ("batch", "per-round"):
                config, mappings, records = self.m12_records(operator, observation)
                workflow.validate_rank_records(records, mappings, "run", self.manifest, config.chain)
                faults = [("scope", "full"), ("implemented_through", "M5"), ("combine_observation", "other")]
                if operator != "dispatch":
                    faults += [("combine_stage_validated", False), ("combine_control_validated", False),
                               ("combine_control_reserved_unchanged", False), ("combine_result_unchanged", False),
                               ("combine_empty", True), ("combine_token_num", 1), ("combine_algorithm_bytes", 0), ("combine_type3_rows", True),
                               ("combine_stage_copy_bytes", 0), ("combine_control_publish_bytes", 0),
                               ("combine_observed_rounds", 0), ("combine_checked_window_mask", 0)]
                for key, value in faults:
                    broken = copy.deepcopy(records)
                    broken[0][key] = value
                    with self.subTest(operator=operator, observation=observation, key=key), self.assertRaises(workflow.Failure):
                        workflow.validate_rank_records(broken, mappings, "run", self.manifest, config.chain)

    def test_sparse_fixture_requires_exact_token_count_and_under64_or_limit_coverage(self):
        for bs, expected_count in ((4, 128), (512, 10000)):
            config, mappings, records = self.m12_records("combine")
            config.chain.update(combine_fixture="sparse", bs=bs, h=516, topk=2, moe_expert_num=32)
            rows = sum(row % 7 == 2 or row % 11 == 5 for row in range(expected_count))
            self.assertEqual(rows < 64, bs == 4)
            record = records[0]
            record.update(config.chain)
            record["chain"].update(config.chain)
            record.update(combine_token_num=expected_count, combine_type3_rows=rows,
                          combine_algorithm_bytes=rows * 1032, combine_stage_copy_bytes=rows * 2064)
            workflow.validate_combine_m12_record(record, mappings[0], config.chain)
            for bad in (expected_count - 1, 10001, True):
                with self.subTest(bs=bs, count=bad), self.assertRaises(workflow.Failure):
                    workflow.validate_combine_m12_record(dict(record, combine_token_num=bad), mappings[0], config.chain)

    def test_fixed_matrix_has_four_complete_subgates_and_five_extra_cases(self):
        cases = workflow.combine_m12_cases()
        self.assertEqual(len(cases), 23)
        self.assertEqual(len({case["name"] for case in cases}), 23)
        for gate in ("F1", "F2", "P", "B"):
            rows = [case for case in cases if case["gate"] == gate]
            self.assertEqual({(row["dtype"], row["operator_mode"]) for row in rows},
                             {(dtype, operator) for dtype in ("bf16", "fp16") for operator in ("combine", "both")})
            self.assertEqual({(row["loops"], row["warmup"]) for row in rows}, {(1, 0)} if gate == "F1" else {(5, 2)})
        self.assertEqual(sum(row["gate"] == "regression" for row in cases), 2)
        self.assertEqual(sum(row["gate"] == "connection" and row["dispatch_quant_mode"] == 4 for row in cases), 2)
        self.assertEqual(sum(row["gate"] == "edges" for row in cases), 3)
        self.assertEqual([row["gate"] for row in cases[-4:]], ["B"] * 4)

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
                code = workflow.combine_m12_commit_validate(config, "gate-" + str(fault))
            self.assertEqual(os.environ, original)
            summary = workflow.read_json(config.logs / ("gate-" + str(fault)) / "summary.json")
            self.assertEqual(code == 0, fault is None)
            names = [row["name"] for row in workflow.combine_m12_cases()]
            self.assertEqual(launched, names[:len(launched)])
            self.assertEqual(summary["expected_cases"], 23)
            self.assertEqual(summary["scope"], "partial")
            if fault is None:
                self.assertEqual(builds, ["profile-off", "profile-on"])
                self.assertEqual(len(summary["cases"]), 23)
                self.assertTrue(all(gate["status"] == "PASS" for gate in summary["gates"].values()))
                self.assertNotEqual(summary["phases"][0]["install_directory"], summary["phases"][1]["install_directory"])
                for index, name in enumerate(names, 1):
                    self.assertIn(f"[{index}/23] PASS {name}", output.getvalue())
            else:
                self.assertNotEqual(summary["gates"]["B"]["status"], "PASS")


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CombineM12Tests(name) for name in CombineM12Tests.__dict__ if name.startswith("test_"))


if __name__ == "__main__":
    unittest.main()
