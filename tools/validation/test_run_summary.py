"""Terminal summaries must retain the verdict without exposing rank logs."""

import unittest

import run_summary


class RunSummaryTests(unittest.TestCase):
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

    def test_pass_and_invalid_breakdown_include_latency_and_trace_in_one_line(self):
        measured = dict(backend="memory", native_status="dispatch", kernel_sha256="a" * 64,
                        algorithm_communication_bytes=None,
                        breakdown=dict(kernel_us=dict(median=2, minimum=1, maximum=3)))
        for status in ("PASS", "FAIL"):
            report = dict(status=status, rank_size=16, completed_ranks=16, measurements=measured,
                          traces=dict(trace_dir="/run/case/traces"),
                          errors=[dict(validation_status="FAIL", communication_completed=True)])
            line = run_summary.render_result(report, "/run/case/summary.json")
            self.assertTrue(line.startswith("VALIDATION_" + status))
            self.assertIn("latency(us) median/min/max=2.000/1.000/3.000", line)
            self.assertIn("bytes=N/A | bandwidth=N/A", line)
            self.assertIn("trace=/run/case/traces", line)
            self.assertEqual(len(line.splitlines()), 1)
