"""用合成四时间戳检查残差符号、路径不对称、异常样本保留和冻结补偿约束。

导入 analyze_clock_residual.py；文件用例写入 TemporaryDirectory，退出时由其清理。
Test analyze_clock_residual.py with synthetic exchanges for sign, asymmetry, retained outliers
and fixed-correction checks; file fixtures live in a TemporaryDirectory cleaned on exit.
"""
import importlib.util
import json
import csv
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("residual", Path(__file__).resolve().parents[1] /
                                            "analyze_clock_residual.py")
residual = importlib.util.module_from_spec(spec)
spec.loader.exec_module(residual)


def sample(theta=100, forward=20, reverse=20, start=1000):
    return dict(status=0, t0=start, t1=start + theta + forward,
                t2=start + theta + forward + 10, t3=start + forward + 10 + reverse,
                rtt_cycles=forward + reverse)


class ResidualTest(unittest.TestCase):
    def test_sign_and_no_half_rtt_substitution(self):
        self.assertEqual(residual.evaluate(sample(), 100)["residual_times_two_cycles"], 0)
        self.assertEqual(residual.evaluate(sample(theta=103), 100)["residual_times_two_cycles"], -6)
        self.assertEqual(residual.evaluate(sample(theta=97), 100)["residual_times_two_cycles"], 6)

    def test_asymmetry_is_not_true_residual(self):
        # True corrected skew is zero; asymmetry alone produces an estimate of -5.
        self.assertEqual(residual.evaluate(sample(forward=30), 100)["residual_times_two_cycles"], -10)

    def test_half_cycle_and_negative_truncation(self):
        self.assertEqual(residual.evaluate(sample(forward=21), 100)["residual_times_two_cycles"], -1)
        self.assertEqual(residual.trunc_half(-201), -100)

    def test_invalid_sample(self):
        row = sample()
        row["rtt_cycles"] += 1
        with self.assertRaises(ValueError):
            residual.evaluate(row, 100)

    def test_heldout_max_keeps_outlier_and_frozen_offset(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manifest = dict(calibration_round=0, validation_first_round=1,
                            cycles_per_second=1000000000, offset_cycles=[0, 100])
            for rank in (0, 1):
                (root / ("frozen_calibration_rank%d.json" % rank)).write_text(json.dumps(manifest))
            rows = []
            for round_id, offsets in enumerate(([100, 100, 100], [101, 102, 5100])):
                for sequence, theta in enumerate(offsets):
                    row = sample(theta=theta, start=1000 + 1000 * round_id + 100 * sequence)
                    row.update(round=round_id, rank=1, sequence=sequence, applied_offset_cycles=100)
                    rows.append(row)
            with (root / "samples_rank1.csv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            result = residual.analyze(root, root / "out")
            self.assertEqual(result["validation_samples"], 3)
            self.assertEqual(result["estimated_max_pairwise_skew_ns"], 5000)
            self.assertIsNone(result["independently_measured_max_clock_skew_ns"])
            rows[-1]["applied_offset_cycles"] = 5100
            with (root / "samples_rank1.csv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            with self.assertRaises(ValueError):
                residual.analyze(root, root / "out")


if __name__ == "__main__":
    unittest.main()
