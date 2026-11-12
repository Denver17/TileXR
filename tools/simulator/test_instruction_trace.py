"""用小型 dump 校验指令配对、公共时钟换算与区间重叠统计。

同一 InstrID 可以在不同 core/pipe 出现；异常记录必须保留计数，不能伪装为完整 Trace。
"""

import pathlib
import tempfile
import unittest

import instruction_trace as trace


def record(time, action, pipe, instr_id, pc=4096, name="testInstruction"):
    return ("[time: %6d] - [%s Instruction Pipe: %s] - "
            "([PC: %d], [InstrID: %d], [InstrName: %s])\n"
            % (time, action, pipe, pc, instr_id, name))


class InstructionTraceTest(unittest.TestCase):
    def test_pairing_identity_and_chrome_time_units(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for core in (0, 1):
                path = pathlib.Path(directory) / ("SU_David_V100_0_aicore%d_aiv0_su.dump" % core)
                path.write_text(record(0, "Popped", "MTE3", 7)
                                + record(100, "Popped", "VEC", 7, pc=8192)
                                + record(200, "Retire", "VEC", 7, pc=8192)
                                + record(300, "Retire", "MTE3", 7))
                paths.append(path)
            events, diagnostics, inputs = trace.parse_dumps(paths)
            self.assertEqual(len(events), 4)
            self.assertFalse(any(diagnostics.values()))
            report = trace.summarize(events, 2.0)
            self.assertEqual(report["core_count"], 2)
            self.assertEqual(report["span_us"], .15)
            self.assertEqual(report["pipelines"]["MTE3"]["mean_union_fraction_of_global_span"], 1)
            self.assertEqual(report["cores"][0]["vec_mte3_overlap_us"], .05)
            chrome = trace.chrome_trace(events, 2.0)
            instructions = [event for event in chrome["traceEvents"] if event["ph"] == "X"]
            self.assertEqual(sum(event["dur"] for event in instructions), .4)
            self.assertEqual(instructions[0]["args"]["pc"], "0x1000")
            self.assertEqual(instructions[0]["args"]["instr_id"], 7)
            self.assertEqual(len(inputs), 2)

    def test_interval_union_and_intersection_do_not_double_count(self):
        union = trace.interval_union([(0, 10), (2, 8), (8, 12), (20, 30)])
        self.assertEqual(trace.interval_length(union), 22)
        self.assertEqual(trace.intersection_length(union, trace.interval_union([(5, 25)])), 12)

    def test_incomplete_and_invalid_pairs_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "SU_David_V100_1_aicore0_aiv1_su.dump"
            path.write_text(record(1, "Popped", "SCALAR", 1)
                            + record(3, "Retire", "SCALAR", 1)
                            + record(4, "Retire", "SCALAR", 2)
                            + record(5, "Popped", "SCALAR", 3)
                            + record(6, "Popped", "SCALAR", 3)
                            + record(10, "Popped", "SCALAR", 4)
                            + record(9, "Retire", "SCALAR", 4)
                            + record(11, "Popped", "SCALAR", 5)
                            + record(12, "Retire", "SCALAR", 5, pc=8192)
                            + "[time: bad] - [Popped Instruction Pipe: SCALAR]\n")
            events, diagnostics, _ = trace.parse_dumps([path])
            self.assertEqual(len(events), 1)
            self.assertEqual(events[0]["chip"], 1)
            for name in ("unmatched_retire", "unmatched_popped", "duplicate_popped",
                         "negative_duration", "identity_mismatch", "malformed_records"):
                self.assertEqual(diagnostics[name], 1)


if __name__ == "__main__":
    unittest.main()
