"""Terminal summaries must retain the verdict without exposing rank logs."""

import tempfile
from pathlib import Path
import unittest

import run_summary


class RunSummaryTests(unittest.TestCase):
    def test_case_analysis_is_optional_and_prints_percentages(self):
        report = dict(status="PASS")
        self.assertNotIn("dedup", run_summary.render_result(report, "/summary.json"))
        report["case_analysis"] = dict(status="complete", min=0, max=75, avg=12.345)
        self.assertIn("dedup(%) min/max/avg=0.00/75.00/12.35",
                      run_summary.render_result(report, "/summary.json"))
        report["case_analysis"] = dict(status="failed")
        self.assertIn("case_analysis=failed", run_summary.render_result(report, "/summary.json"))

    def test_failure_categories_do_not_confuse_capture_or_cleanup_with_correctness(self):
        invalid = dict(type="RESULT_FAILED", validation_status="FAIL", communication_completed=True)
        timeout = dict(type="EXIT_FAILED", timeout=True)
        cases = [([invalid], "结果校验失败"), ([timeout], "执行卡死（超时）"),
                 ([dict(type="EXIT_FAILED")], "执行阶段失败"),
                 ([invalid, dict(type="CLEANUP_FAILED")], "执行阶段失败"),
                 ([dict(type="LOG_COLLECT_FAILED", timeout=True)], "执行阶段失败"),
                 ([dict(type="RESULT_FAILED", communication_completed=True)], "执行阶段失败")]
        for errors, label in cases:
            with self.subTest(errors=errors):
                errors[0]["message"] = "large rank log\nsecond line"
                report = dict(status="FAIL", primary_error=errors[0], errors=errors)
                line = run_summary.render_result(report, "/run/case/summary.json")
                self.assertIn("VALIDATION_FAIL " + label, line)
                self.assertEqual(len(line.splitlines()), 1)
                self.assertNotIn("large rank log", line)

    def test_pass_and_invalid_breakdown_keep_verdict_and_separate_performance_line(self):
        measured = dict(backend="memory", native_status="dispatch", kernel_sha256="a" * 64,
                        algorithm_communication_bytes=None,
                        breakdown=dict(kernel_us=dict(median=2, minimum=1, maximum=3)))
        for status in ("PASS", "FAIL"):
            report = dict(status=status, rank_size=16, completed_ranks=16, measurements=measured,
                          traces=dict(trace_dir="/run/case/traces"),
                          errors=[dict(validation_status="FAIL", communication_completed=True)])
            line = run_summary.render_result(report, "/run/case/summary.json")
            self.assertTrue(line.startswith("VALIDATION_" + status))
            self.assertIn("latency(us) min/median/max=1.000/2.000/3.000", line)
            self.assertIn("bytes=N/A | bandwidth=N/A", line)
            self.assertIn("trace=/run/case/traces", line)
            self.assertEqual(len(line.splitlines()), 2)
            self.assertTrue(line.splitlines()[1].startswith("dispatch:"))

    def test_performance_prints_server_min_mean_max_and_selected_sample_count(self):
        stats = dict(maximum_us=9, minimum_us=1, mean_us=4, median_us=2, samples_per_rank=50)
        measured = dict(backend="memory", native_status="dispatch", kernel_sha256="b" * 64,
                        algorithm_communication_bytes=3000, performance=stats,
                        bandwidth_gbps=dict(maximum=5, minimum=1, mean=3, median=2))
        line = run_summary.render_result(dict(status="PASS", measurements=measured), "/run/summary.json")
        self.assertIn("samples/card=50", line)
        self.assertIn("server_latency(us) min/mean/max=1.000/4.000/9.000", line)
        self.assertIn("server_bandwidth(GB/s) min/mean/max=1.000/3.000/5.000", line)
        self.assertNotIn("median", line)
        self.assertEqual(len(line.splitlines()), 2)

    @staticmethod
    def operator_summary(operator, mode):
        summary = dict(backend="memory", native_status=operator, kernel_sha256="c" * 64,
                       algorithm_communication_bytes=0)
        if mode == "performance":
            summary[mode] = dict(maximum_us=9, minimum_us=1, mean_us=4, samples_per_rank=20)
        else:
            summary[mode] = dict(kernel_us=dict(median=2, minimum=1, maximum=3))
        return summary

    def test_operator_modes_render_only_selected_operators_on_separate_lines(self):
        for mode in ("performance", "breakdown"):
            for operator, expected in (("dispatch", ["dispatch"]), ("combine", ["combine"]),
                                       ("both", ["dispatch", "combine"])):
                with self.subTest(mode=mode, operator=operator):
                    config = dict(run_mode=mode, operator_mode=operator, bs=96, loops=20, warmup=5)
                    summaries = {op: self.operator_summary(op, mode) for op in reversed(expected)}
                    report = dict(status="PASS", chain_configuration=config, operator_measurements=summaries)
                    lines = run_summary.render_result(report, "/summary.json").splitlines()
                    self.assertEqual(len(lines), 1 + len(expected))
                    self.assertNotIn("latency", lines[0])
                    for line, op in zip(lines[1:], expected):
                        self.assertTrue(line.startswith(op + ":"), line)
                        self.assertIn("latency(us)", line)
                        self.assertIn("native=" + op, line)

    def test_dual_schema_does_not_duplicate_dispatch_or_render_inactive_operator(self):
        report = dict(status="PASS", chain_configuration=dict(run_mode="performance", operator_mode="combine",
                      bs=96, loops=20, warmup=5), measurements=self.operator_summary("dispatch", "performance"),
                      operator_measurements={op: self.operator_summary(op, "performance") for op in ("dispatch", "combine")})
        lines = run_summary.render_result(report, "/summary.json").splitlines()
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[1].startswith("combine:"))
        report["chain_configuration"]["operator_mode"] = "both"
        lines = run_summary.render_result(report, "/summary.json").splitlines()
        self.assertEqual(sum(line.startswith("dispatch:") for line in lines), 1)

    def test_partial_failure_keeps_only_available_operator_measurement_and_scope(self):
        combine = self.operator_summary("combine", "performance")
        combine.update(scope="partial", implemented_through="M2", wait_us=50, algorithm_bytes=2048,
                       stage_copy_bytes=4096, control_publish_bytes=1024, algorithm_bandwidth_gbps=None)
        report = dict(status="FAIL", chain_configuration=dict(run_mode="performance", operator_mode="both",
                      bs=96, loops=20, warmup=5), operator_measurements=dict(combine=combine))
        lines = run_summary.render_result(report, "/summary.json").splitlines()
        self.assertTrue(lines[0].startswith("VALIDATION_FAIL"))
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[1].startswith("combine:"))
        for field in ("scope=partial", "implemented_through=M2", "wait_us=50", "algorithm_payload=2048B",
                      "algorithm_payload_GB/s=N/A"):
            self.assertIn(field, lines[1])
        combine["algorithm_bandwidth_gbps"] = dict(minimum=1, median=2, maximum=3)
        self.assertIn("algorithm_payload_GB/s(nonzero-work ranks) min/median/max=1.000/2.000/3.000",
                      run_summary.render_result(report, "/summary.json"))
        combine["wait_us"] = 0
        self.assertIn("wait_us=0", run_summary.render_result(report, "/summary.json"))

    def test_functional_has_no_performance_line(self):
        report = dict(status="PASS", chain_configuration=dict(run_mode="functional", operator_mode="both",
                      bs=96, loops=20, warmup=5), operator_measurements={})
        self.assertEqual(len(run_summary.render_result(report, "/summary.json").splitlines()), 1)

    def test_write_result_preserves_intentional_lines_and_sanitizes_embedded_newlines(self):
        report = dict(status="PASS", operator_measurements={op: self.operator_summary(op, "performance")
                      for op in ("dispatch", "combine")})
        report["operator_measurements"]["combine"]["native_status"] = "combine\nextra\rline"
        with tempfile.TemporaryDirectory() as folder:
            run_summary.write_result(folder, report, "/summary.json")
            lines = (Path(folder) / "result.txt").read_text().splitlines()
            self.assertEqual(len(lines), 3)
            self.assertIn("native=combine extra line", lines[2])
            self.assertFalse((Path(folder) / "result.txt.tmp").exists())
