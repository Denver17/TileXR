"""Test benchmark baseline and ABI-v1 launch trace accounting offline."""

import unittest
from check_relay import MODES, check_launches


def events():
    records = []
    for index, mode in enumerate(MODES):
        offset = index * 10
        records.append({"ph": "X", "name": mode + "/round0", "ts": offset, "dur": 10})
        names = ["main_kernel"] if mode == "generic_relay" else ["TileXRAllGather_float16_t", "main_kernel"]
        for tile, name in enumerate(names):
            records.append({"ph": "X", "name": name, "ts": offset + tile + 1, "dur": 1,
                            "args": {"Task Type": "KERNEL_AIVEC" if tile == 0 and mode != "generic_relay" else "KERNEL_MIX_AIC"}})
    return records


class RelayTraceTests(unittest.TestCase):
    def test_prepared_execution_interleaved_rounds(self):
        records = [
            {"ph": "X", "name": "generic_execution/round0", "ts": 0, "dur": 10},
            {"ph": "X", "name": "main_kernel", "ts": 1, "dur": 1,
             "args": {"Task Type": "KERNEL_MIX_AIC"}},
            {"ph": "X", "name": "generic_relay/round0", "ts": 10, "dur": 10},
            {"ph": "X", "name": "main_kernel", "ts": 11, "dur": 1,
             "args": {"Task Type": "KERNEL_MIX_AIC"}}]
        modes = ("generic_relay", "generic_execution")
        self.assertEqual(check_launches(records, 128, 0, 1, modes),
                         {"generic_relay": 1, "generic_execution": 1})
        with self.assertRaisesRegex(ValueError, "expected 1"):
            check_launches(records + [records[1]], 128, 0, 1, modes)

    def test_framework_preparation_is_not_a_pipeline_launch(self):
        fill = {"ph": "X", "name": "aclnnInplaceFillScalar_FillAiCore_Fill", "ts": 1.5,
                "dur": 1, "args": {"Task Type": "KERNEL_AIVEC"}}
        self.assertEqual(check_launches(events() + [fill], 128, 0, 1)["serial"], 2)

    def test_missing_round(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            check_launches(events()[:-2], 128, 0, 1)

    def test_duplicate_round(self):
        with self.assertRaisesRegex(ValueError, "duplicated"):
            check_launches(events() + [events()[0]], 128, 0, 1)

    def test_extra_relay_launch(self):
        with self.assertRaisesRegex(ValueError, "expected 1"):
            check_launches(events() + [events()[-1]], 128, 0, 1)

    def test_wrong_baseline_kernel(self):
        records = events(); records[1]["name"] = "other_kernel"
        with self.assertRaisesRegex(ValueError, "wrong baseline"):
            check_launches(records, 128, 0, 1)


if __name__ == "__main__":
    unittest.main()
