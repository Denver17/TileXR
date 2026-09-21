"""P2.1 resumes original package evidence; never rebuilds or reruns OFF cases."""
import copy
from contextlib import redirect_stderr, redirect_stdout
import io
import os
from pathlib import Path
import unittest
from unittest import mock

import workflow
from test_workflow import TemporaryRoot


class CombineM12ResumeTests(TemporaryRoot):
    def setUp(self):
        super().setUp()
        self.hostfile.write_text("192.0.2.1\n192.0.2.2\n")
        os.environ.update(TILEXR_VALIDATION_CHECKPOINT=workflow.COMBINE_M12_CHECKPOINT,
                          TILEXR_VALIDATION_BUILD_MODE="production")
        self.old = self.root / "run/validation/original/summary.json"
        self.hosts = workflow.hosts_from_file(self.hostfile)
        self.manifests = {}
        phases = []
        for profiling in (False, True):
            package = "profile-on" if profiling else "profile-off"
            with mock.patch.dict(os.environ, TILEXR_VALIDATION_BREAKDOWN=str(int(profiling))):
                cfg = workflow.Config("build")
            artifact = cfg.install / "lib/kernel.o"
            artifact.parent.mkdir(parents=True)
            artifact.write_text(package)
            manifest = dict(build_id=package, checkpoint=cfg.checkpoint, mode="production",
                architecture=workflow.platform.machine(), server_dedup_profiling=profiling,
                server_dedup_profile_schema=workflow.measurements.RECORDS_SCHEMA,
                server_dedup_combine_contract=workflow.COMBINE_M12_ABI,
                server_dedup_contract=workflow.DISPATCH_ABI,
                source=dict(commit=None, provenance="file-copy", content_sha256=("b" if profiling else "a") * 64),
                artifacts=workflow.artifact_files(cfg.install))
            workflow.write_json(cfg.install / "manifest.json", manifest)
            self.manifests[package] = manifest
            phase = dict(package=package, profiling=profiling, build_id=package,
                         status="FAIL" if profiling else "PASS", build_logs="retained-build-logs",
                         install_directory=str(cfg.install))
            if not profiling:
                phase.update(manifest=str(self.old.parent / "builds/off.json"),
                             host_regression=str(self.old.parent / "off-host.json"))
                workflow.write_json(phase["manifest"], manifest)
                workflow.write_json(phase["host_regression"], dict(status="PASS", checkpoint=cfg.checkpoint,
                                                                build_id=package))
            phases.append(phase)
        cases = workflow.combine_m12_cases()
        rows = []
        for case in cases[:19]:
            values = dict(workflow.combine_m12_defaults(), **{k: v for k, v in case.items() if k not in ("name", "gate")})
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_" + k.upper(): str(v) for k, v in values.items()}):
                chain = workflow.Config("run").chain
            path = self.old.parent / "cases" / (case["name"] + ".json")
            workflow.write_json(path, dict(status="PASS", checkpoint=workflow.COMBINE_M12_CHECKPOINT,
                build_id="profile-off", rank_size=16, completed_ranks=16, expected_hosts=self.hosts,
                chain_configuration=chain, errors=[]))
            rows.append(dict(name=case["name"], gate=case["gate"], status="PASS", completed_ranks=16,
                             build_id="profile-off", configuration=chain, summary=str(path)))
        self.previous = dict(checkpoint=workflow.COMBINE_M12_CHECKPOINT, mode="production", status="FAIL",
            cleanup="complete", rank_size=16, expected_hosts=self.hosts, cases=rows, case_manifest=cases, phases=phases,
            errors=[dict(type="BUILD_FAILED", stage="build-profile-on",
                         message="P2.1 OFF/ON源码不一致，拒绝混合版本验收")])
        workflow.write_json(self.old, self.previous)
        os.environ["TILEXR_VALIDATION_RESUME_SUMMARY"] = str(self.old)

    def invoke(self, fault=None, name="resumed"):
        config = workflow.Config("commit-validate")
        launched, host_calls, sync_calls = [], [], []
        output = io.StringIO()
        def host(cfg, run_id):
            host_calls.append(cfg.install.name)
            return int(fault == "host")
        def sync(cfg):
            sync_calls.append(cfg.install.name)
            if fault == "sync":
                raise ValueError("sync failed")
        def run(cfg):
            launched.append(cfg.logs.name)
            cfg.last_summary_path = cfg.logs / "summary.json"
            workflow.write_json(cfg.last_summary_path, dict(status="FAIL" if fault == "case" else "PASS",
                checkpoint=config.checkpoint, build_id="profile-on", rank_size=16,
                completed_ranks=15 if fault == "rank" else 16))
            return int(fault == "case")
        environment = dict(os.environ)
        with mock.patch.object(workflow, "build", side_effect=AssertionError("resume must not rebuild")) as build, \
                mock.patch.object(workflow, "host_validate", side_effect=host), \
                mock.patch.object(workflow, "sync", side_effect=sync), mock.patch.object(workflow, "run", side_effect=run), \
                redirect_stdout(output), redirect_stderr(io.StringIO()):
            code = workflow.combine_m12_commit_validate(config, name)
        build.assert_not_called()
        self.assertEqual(os.environ, environment)
        summary = workflow.read_json(config.logs / name / "summary.json")
        return code, summary, launched, host_calls, sync_calls, output.getvalue()

    def test_resume_skips_19_cases_and_both_builds_but_runs_on_host_sync_and_four_cases(self):
        old_bytes = self.old.read_bytes()
        code, summary, launched, host, sync, output = self.invoke()
        self.assertEqual(code, 0)
        self.assertEqual(launched, [c["name"] for c in workflow.combine_m12_cases()[19:]])
        self.assertEqual(host, ["validation-combine-p21-profile-on"])
        self.assertEqual(sync, host)
        self.assertEqual(summary["status"], "PASS")
        self.assertEqual(len(summary["cases"]), 23)
        self.assertTrue(all(gate["status"] == "PASS" for gate in summary["gates"].values()))
        self.assertEqual(sum(row.get("reused", False) for row in summary["cases"]), 19)
        for row in summary["cases"][:19]:
            self.assertEqual(workflow.digest(row["summary"]), row["summary_sha256"])
        self.assertEqual(summary["resume"]["original_errors"], self.previous["errors"])
        self.assertNotEqual(summary["resume"]["source_identities"]["profile-off"],
                            summary["resume"]["source_identities"]["profile-on"])
        self.assertNotIn("source_identity", summary)
        self.assertEqual(self.old.read_bytes(), old_bytes)
        self.assertEqual(output.count("] SKIP "), 19)
        self.assertIn("[20/23] START B-combine-bf16", output)

    def test_resume_rejects_incomplete_wrong_or_incompatible_historical_evidence(self):
        mutations = {
            "checkpoint": lambda p: p.update(checkpoint=workflow.COMBINE_PROFILE_CHECKPOINT),
            "cleanup": lambda p: p.update(cleanup="failed"),
            "missing-case": lambda p: p["cases"].pop(),
            "duplicate-case": lambda p: p["cases"].__setitem__(1, p["cases"][0]),
            "failed-case": lambda p: p["cases"][0].update(status="FAIL"),
            "wrong-build": lambda p: p["cases"][0].update(build_id="other"),
            "wrong-shape": lambda p: p["cases"][0]["configuration"].update(h=1),
            "wrong-ranks": lambda p: p["cases"][0].update(completed_ranks=15),
            "wrong-hosts": lambda p: p.update(expected_hosts=[]),
            "wrong-matrix": lambda p: p.update(case_manifest=[]),
            "wrong-failure": lambda p: p["errors"][0].update(stage="B-combine-bf16"),
            "new-on-package": lambda p: p["phases"][1].update(build_id="other"),
            "missing-report": lambda p: p["cases"][0].update(summary=str(self.root / "missing.json")),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                previous = copy.deepcopy(self.previous)
                mutate(previous)
                workflow.write_json(self.old, previous)
                code, summary, launched, host, sync, _ = self.invoke(name=name)
                self.assertNotEqual(code, 0)
                self.assertEqual((launched, host, sync), ([], [], []))
                self.assertEqual(summary["errors"][0]["stage"], "resume-preflight")

    def test_resume_rechecks_actual_package_hashes_and_case_reports(self):
        targets = [self.root / "install" / ("validation-combine-p21-" + mode) / "lib/kernel.o"
                   for mode in ("profile-off", "profile-on")]
        targets.append(Path(self.previous["cases"][0]["summary"]))
        for index, target in enumerate(targets):
            with self.subTest(target=str(target)):
                original = target.read_bytes()
                if target.suffix == ".json":
                    report = workflow.read_json(target)
                    report["completed_ranks"] = 15
                    workflow.write_json(target, report)
                else:
                    target.write_text("changed binary")
                code, _, launched, host, sync, _ = self.invoke(name="changed-" + str(index))
                self.assertNotEqual(code, 0)
                self.assertEqual((launched, host, sync), ([], [], []))
                target.write_bytes(original)

    def test_resume_stops_on_new_host_sync_or_breakdown_failure(self):
        for fault in ("host", "sync", "case", "rank"):
            with self.subTest(fault=fault):
                code, summary, launched, _, _, _ = self.invoke(fault, name=fault)
                self.assertNotEqual(code, 0)
                self.assertEqual(len(launched), int(fault in ("case", "rank")))
                self.assertNotEqual(summary["gates"]["B"]["status"], "PASS")


if __name__ == "__main__":
    unittest.main()
