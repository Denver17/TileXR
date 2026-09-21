#!/usr/bin/env python3
"""C6 workflow: isolated captures, exact active operators and two-build delivery."""

import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import threading
import time
import unittest
from unittest import mock

import workflow
import test_combine_multi as c5


class CombineProfileTests(c5.CombineMultiTests):
    def setUp(self):
        super().setUp()
        os.environ["TILEXR_INTERNAL_ACCEPTANCE_STAGE"] = workflow.COMBINE_PROFILE_CHECKPOINT
        self.manifest["source"]["content_sha256"] = "a" * 64
        self.manifest.update(checkpoint=workflow.COMBINE_PROFILE_CHECKPOINT, mode="production",
                             artifacts={f"lib/tilexr/kernels/tilexr_ep_{operator}_memory_server_dedup_kernel.o": value * 64
                                        for operator, value in (("dispatch", "d"), ("combine", "c"))})

    def profile_records(self, operator="both", mode="breakdown"):
        config, mappings, records = super().multi_records(operator)
        config.chain["run_mode"] = mode
        if mode == "performance":
            config.chain["msprof"] = "msprof"
        for record in records:
            record["run_mode"] = mode
            record["chain"]["run_mode"] = mode
            if "dispatch" in record:
                record["dispatch"]["run_mode"] = mode
        return config, mappings, records

    def breakdown_markers(self, mapping, record, config, paths, capture="capture"):
        return [dict(workflow.rank_record(mapping, "run", self.manifest), pid=record["pid"],
                     operator=operator, schema=workflow.measurements.RECORDS_SCHEMA,
                     records_path=str(paths[operator]), capture_id=capture, launches=7,
                     capture_mode="latest_launch", captured_launches=1, captured_launch=6)
                for operator in workflow.operator_measure.active_operators(config.chain)]

    def test_c6_configuration_accepts_three_modes_and_preserves_old_gates(self):
        for operator in ("dispatch", "combine", "both"):
            for mode in ("functional", "performance", "breakdown"):
                with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": operator,
                                                 "TILEXR_VALIDATION_RUN_MODE": mode}):
                    config = self.config()
                    self.assertEqual(config.install.name, "validation-combine-c6")
                    self.assertEqual(config.chain["run_mode"], mode)
                    self.assertEqual((config.chain["loops"], config.chain["warmup"]), (5, 2))
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_BUILD_MODE": "host"}), self.assertRaises(ValueError):
            self.config()
        for checkpoint in (workflow.COMBINE_CHAIN_CHECKPOINT, workflow.COMBINE_MULTI_CHECKPOINT):
            with mock.patch.dict(os.environ, {"TILEXR_INTERNAL_ACCEPTANCE_STAGE": checkpoint,
                    "TILEXR_VALIDATION_RUN_MODE": "performance"}), self.assertRaises(ValueError):
                self.config()
        self.assertEqual(len(workflow.COMBINE_TESTS[workflow.COMBINE_PROFILE_CHECKPOINT]), 11)

    def test_breakdown_requires_unique_enabled_operator_and_exact_capture_identity(self):
        config, mappings, records = self.profile_records()
        paths = {operator: self.root / (operator + ".records.v1") for operator in ("dispatch", "combine")}
        markers = self.breakdown_markers(mappings[0], records[0], config, paths)
        def validate(rows, extra=""):
            output = "".join(workflow.OPERATOR_BREAKDOWN_MARKER + json.dumps(row) + "\n" for row in rows) + extra
            return workflow.parse_operator_breakdowns(output, mappings[0], "run", self.manifest, records[0],
                                                       config.chain, paths, "capture")
        self.assertEqual(set(validate(markers)), {"dispatch", "combine"})
        for rows in (markers[:1], markers + markers[:1], [dict(markers[0], operator="inactive"), markers[1]]):
            with self.assertRaises(workflow.Failure):
                validate(rows)
        for key, value in (("pid", 99), ("records_path", str(paths["dispatch"])), ("capture_id", "old"),
                           ("launches", True), ("launches", 5), ("captured_launch", 5), ("rank", 8),
                           ("schema", "old"), ("checkpoint", workflow.COMBINE_MULTI_CHECKPOINT)):
            changed = copy.deepcopy(markers)
            changed[1][key] = value
            with self.subTest(key=key), self.assertRaises(workflow.Failure):
                validate(changed)
        with self.assertRaises(workflow.Failure):
            validate(markers, workflow.BREAKDOWN_MARKER + "{}\n")
        config.chain["operator_mode"] = "dispatch"
        with self.assertRaises(workflow.Failure):
            validate(markers)
        config.chain["run_mode"] = "performance"
        with self.assertRaises(workflow.Failure):
            validate(markers[:1])
        self.assertEqual(validate([]), {})

    def test_rank_launch_uses_one_profiler_and_parses_only_active_operator_regions(self):
        for operator in ("dispatch", "combine", "both"):
            for mode in ("functional", "performance", "breakdown"):
                config, mappings, records = self.profile_records(operator, mode)
                active = workflow.operator_measure.active_operators(config.chain)
                def execute(argv, log, **kwargs):
                    env = kwargs["env"]
                    rank = int(log.stem.split("-")[1])
                    self.assertEqual(argv[0] == "msprof", mode == "performance")
                    for op, key in (("dispatch", "TILEXR_PROFILING_RECORDS_PATH"),
                                    ("combine", "TILEXR_PROFILING_COMBINE_RECORDS_PATH")):
                        self.assertEqual(key in env, mode == "breakdown" and op in active)
                        if key in env:
                            self.assertEqual(env[key], str(log.parent / f"rank-{rank}.{op}.records.v1"))
                    record = copy.deepcopy(records[rank])
                    chain = record.pop("chain")
                    dispatch = record.pop("dispatch", None)
                    output = workflow.CHAIN_MARKER + json.dumps(chain) + "\n"
                    output += (workflow.DISPATCH_MARKER if operator == "dispatch" else workflow.COMBINE_CHAIN_MARKER) + json.dumps(record) + "\n"
                    if dispatch:
                        output += workflow.DISPATCH_MARKER + json.dumps(dispatch) + "\n"
                    if mode == "breakdown":
                        paths = {op: log.parent / f"rank-{rank}.{op}.records.v1" for op in active}
                        for marker in self.breakdown_markers(mappings[rank], record, config, paths,
                                                              env["TILEXR_PROFILING_CAPTURE_ID"]):
                            output += workflow.OPERATOR_BREAKDOWN_MARKER + json.dumps(marker) + "\n"
                    return output + 'EP_SERVER_DEDUP_WALL ' + json.dumps(dict(
                        rank=rank, unit="seconds", total=0, stages={"reference-build": 0})) + '\n'
                result = dict(errors=[])
                with self.subTest(operator=operator, mode=mode), mock.patch.dict(os.environ, {
                        "TILEXR_PROFILING_RECORDS_PATH": "/stale/dispatch", "TILEXR_PROFILING_COMBINE_RECORDS_PATH": "/stale/combine"}), \
                        mock.patch.object(workflow, "execute", side_effect=execute), \
                        mock.patch.object(workflow.operator_measure, "performance_record", return_value={}) as performance, \
                        mock.patch.object(workflow.operator_measure, "breakdown_record", return_value={}) as breakdown, \
                        mock.patch.object(workflow.operator_measure, "validate_operator_measurements") as validate:
                    workflow.run_ranks(config, "run", self.manifest, mappings, time.monotonic() + 60,
                                       threading.Event(), result)
                self.assertEqual(result["errors"], [], (operator, mode, result))
                self.assertEqual(len(result["ranks"]), 8)
                self.assertEqual([row["rank"] for row in result["rank_wall_times"]], list(range(8)))
                for timing in result["rank_wall_times"]:
                    self.assertEqual(timing["consumer"]["status"], "recorded")
                    self.assertEqual(timing["consumer"]["rank"], timing["rank"])
                    phases = timing["stages"]
                    self.assertGreaterEqual(phases["process"], 0)
                    self.assertEqual("profile_decode" in phases, mode != "functional")
                    self.assertEqual("profile_wait" in phases, mode == "breakdown")
                    self.assertAlmostEqual(sum(phases.values()), timing["total"])
                self.assertEqual(performance.call_count, 8 * len(active) if mode == "performance" else 0)
                self.assertEqual(breakdown.call_count, 8 * len(active) if mode == "breakdown" else 0)
                self.assertEqual(validate.call_count, 8)
                for record in result["ranks"]:
                    self.assertEqual(set(record["operator_measurements"]), set() if mode == "functional" else set(active))
                for call in breakdown.call_args_list:
                    op = call.kwargs["operator"]
                    self.assertEqual(Path(call.args[4]).name, op)
                    self.assertEqual(call.kwargs["kernel_sha"], ("d" if op == "dispatch" else "c") * 64)
                    self.assertEqual(Path(call.kwargs["mapping_path"]).name,
                                     "server_dedup.mapping.json" if op == "dispatch" else "server_dedup_combine.mapping.json")

    def test_partial_capture_failure_retains_completed_operator_evidence(self):
        for operator in ("dispatch", "combine"):
            with self.subTest(failed_operator=operator):
                self.check_partial_capture_failure(operator)

    def test_failed_process_keeps_partial_consumer_timing_without_changing_failure(self):
        config, mappings, _ = self.profile_records()
        def execute(argv, log, **kwargs):
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text('EP_SERVER_DEDUP_WALL {"rank":0,"unit":"seconds","total":0,'
                           '"stages":{"communicator":0}}\n')
            raise workflow.Failure("EXIT_FAILED", "communicator failed", returncode=1)
        result = dict(errors=[])
        with mock.patch.object(workflow, "execute", side_effect=execute):
            workflow.run_ranks(config, "run", self.manifest, mappings[:1], time.monotonic() + 60,
                               threading.Event(), result)
        self.assertEqual(result["ranks"], [])
        self.assertEqual(result["errors"][0]["type"], "EXIT_FAILED")
        self.assertEqual(result["rank_wall_times"][0]["consumer"]["status"], "recorded")
        self.assertEqual(result["rank_wall_times"][0]["consumer"]["stages"], dict(communicator=0))

    def check_partial_capture_failure(self, failed_operator):
        config, mappings, records = self.profile_records()
        mapping = mappings[0]
        def execute(argv, log, **kwargs):
            record = dict(records[0])
            chain = record.pop("chain")
            dispatch = record.pop("dispatch")
            paths = {op: log.parent / f"rank-0.{op}.records.v1" for op in ("dispatch", "combine")}
            markers = self.breakdown_markers(mapping, record, config, paths, kwargs["env"]["TILEXR_PROFILING_CAPTURE_ID"])
            return (workflow.CHAIN_MARKER + json.dumps(chain) + "\n" + workflow.DISPATCH_MARKER + json.dumps(dispatch) + "\n"
                    + workflow.COMBINE_CHAIN_MARKER + json.dumps(record) + "\n" + "".join(
                    workflow.OPERATOR_BREAKDOWN_MARKER + json.dumps(marker) + "\n" for marker in markers) +
                    'EP_SERVER_DEDUP_WALL {"rank":0,"unit":"seconds","total":0,"stages":{"reference-build":0}}\n')
        def parse(*args, **kwargs):
            if kwargs["operator"] == failed_operator:
                raise ValueError(failed_operator + " capture corrupt")
            return dict(status="complete", summary_path=kwargs["operator"] + "/summary.json")
        result = dict(errors=[])
        with mock.patch.object(workflow, "execute", side_effect=execute), \
                mock.patch.object(workflow.operator_measure, "breakdown_record", side_effect=parse):
            workflow.run_ranks(config, "run", self.manifest, [mapping], time.monotonic() + 60,
                               threading.Event(), result)
        self.assertTrue(result["errors"])
        self.assertEqual(len(result["rank_wall_times"]), 1)
        self.assertIn("profile_decode", result["rank_wall_times"][0]["stages"])
        self.assertEqual(result["rank_wall_times"][0]["consumer"]["status"], "recorded")
        failure = result["profiling_failures"][0]
        self.assertTrue(failure["communication_completed"])
        self.assertEqual(set(failure["communication"]["operator_measurements"]), {"dispatch", "combine"} - {failed_operator})
        self.assertEqual(set(failure["operator_records_paths"]), {"dispatch", "combine"})

    def test_two_phase_gate_keeps_build_provenance_and_stops_at_first_failure(self):
        for fault in (None, "build-performance", "build-breakdown", "host-performance", "sync-breakdown", "performance-combine", "breakdown-both", "wrong-build", "source-change"):
            config = workflow.Config("commit-validate")
            builds, cases = [], []
            current_manifest = copy.deepcopy(self.manifest)
            def build(cfg):
                mode = "breakdown" if cfg.profiling else "performance"
                cfg.build_id = mode + "-build"
                builds.append(mode)
                if fault == "build-" + mode:
                    raise ValueError("injected build failure")
                current_manifest.update(build_id=cfg.build_id, server_dedup_profiling=cfg.profiling)
                if fault == "source-change" and cfg.profiling:
                    current_manifest["source"] = dict(current_manifest["source"], content_sha256="b" * 64)
            def host(cfg, run_id):
                return int(fault == ("host-breakdown" if cfg.profiling else "host-performance"))
            def sync(cfg):
                if fault == "sync-" + cfg.chain["run_mode"]:
                    raise ValueError("injected sync failure")
                self.assertEqual(len(cfg.hosts()), 2)
            def run(cfg):
                name = cfg.chain["run_mode"] + "-" + cfg.chain["operator_mode"]
                cases.append(name)
                cfg.last_summary_path = cfg.logs / "run/summary.json"
                workflow.write_json(cfg.last_summary_path, dict(status="FAIL" if name == fault else "PASS",
                    build_id="wrong" if fault == "wrong-build" else current_manifest["build_id"], rank_size=16, completed_ranks=16))
                return int(name == fault)
            output = io.StringIO()
            before = dict(os.environ)
            with self.subTest(fault=fault), mock.patch.object(workflow, "build", side_effect=build), \
                    mock.patch.object(workflow, "verify_build", side_effect=lambda *a, **kw: copy.deepcopy(current_manifest)), \
                    mock.patch.object(workflow, "host_validate", side_effect=host), mock.patch.object(workflow, "sync", side_effect=sync), \
                    mock.patch.object(workflow, "run", side_effect=run), redirect_stdout(output), redirect_stderr(io.StringIO()):
                code = workflow.combine_profile_commit_validate(config, "gate-" + str(fault))
            self.assertEqual(os.environ, before)
            report = workflow.read_json(config.logs / ("gate-" + str(fault)) / "summary.json")
            self.assertEqual(code == 0, fault is None)
            self.assertEqual(report["expected_cases"], 6)
            expected = [mode + "-" + op for mode in ("performance", "breakdown") for op in ("dispatch", "combine", "both")]
            self.assertEqual(cases, expected[:len(cases)])
            if fault is None:
                self.assertEqual(builds, ["performance", "breakdown"])
                self.assertEqual(len(cases), 6)
                for phase in report["phases"]:
                    saved = workflow.read_json(phase["manifest"])
                    self.assertEqual(saved["build_id"], phase["build_id"])
                    self.assertEqual(saved["server_dedup_profiling"], phase["profiling"])
                    self.assertEqual(phase["status"], "PASS")
                    self.assertTrue(phase["build_directory"].endswith("-profile-on" if phase["profiling"] else "-profile-off"))
            if fault in ("host-performance", "wrong-build", "performance-combine"):
                self.assertEqual(builds, ["performance"])
            for index, name in enumerate(cases, 1):
                self.assertIn(f"[{index}/6] START {name}", output.getvalue())
                self.assertIn(f"[{index}/6] {'FAIL' if name == fault or fault == 'wrong-build' else 'PASS'} {name}", output.getvalue())

    def test_relocation_keeps_enabled_operator_report_directories_separate(self):
        result = dict(ranks=[dict(rank=0, operator_measurements={op: dict(breakdown=dict(records_path=f"old/{op}"))
                            for op in ("dispatch", "combine")})], errors=[], profiling_failures=[])
        workflow.rebase_node_breakdown_reports(result, self.root / "node", "192.0.2.1", local_records=True)
        for op, item in result["ranks"][0]["operator_measurements"].items():
            evidence = item["breakdown"]
            self.assertEqual(Path(evidence["summary_path"]).parent.name, op)
            self.assertEqual(Path(evidence["local_records_path"]).name, f"rank-0.{op}.records.v1")


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(CombineProfileTests(name) for name in CombineProfileTests.__dict__ if name.startswith("test_"))


if __name__ == "__main__":
    unittest.main()
