"""Test benchmark overlap analysis with synthetic traces, without an NPU."""

import unittest

from check_overlap import analyze, intersection


def task(name, start, duration, pid=100):
    comm = name.startswith("TileXR")
    return {"name": name, "ts": str(start), "dur": duration, "ph": "X", "pid": pid,
            "tid": 1 if comm else 2,
            "args": {"Task Type": "KERNEL_AIVEC" if comm else "KERNEL_MIX_AIC"}}


def trace():
    events = [{"name": mode + "/round0", "ts": str(start), "dur": 10, "ph": "X"}
              for mode, start in [("serial", 0), ("serial_tiled", 10), ("overlap", 20)]]
    for offset in (0, 10, 20):
        events += [task("TileXRAllGather_float16_t", offset + 1, 1),
                   task("main_kernel", offset + 2, 4)]
    events += [task("TileXRAllGather_float16_t", 23, 1), task("main_kernel", 26, 3)]
    return events


class OverlapTests(unittest.TestCase):
    def test_trace_order_does_not_change_tile_order(self):
        events = trace()
        self.assertEqual(analyze(events[:3] + list(reversed(events[3:]))), analyze(events))

    def test_future_tile_and_serial_controls(self):
        rows = analyze(trace())
        self.assertEqual([r["overlap_us"] for r in rows], [0, 0, 1])
        self.assertEqual(rows[-1]["pairs"][0]["communication_tile"], 1)

    def test_other_device_is_not_overlap(self):
        events = trace()
        events[-2]["pid"] = 200
        with self.assertRaisesRegex(ValueError, "no hardware overlap"):
            analyze(events)

    def test_consumer_before_ready_is_rejected(self):
        events = trace()
        events[-4]["dur"] = 3
        with self.assertRaisesRegex(ValueError, "its own"):
            analyze(events)

    def test_missing_compute_task_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unequal"):
            analyze(trace()[:-1])

    def test_timestamp_precision_and_touching_intervals(self):
        a = task("main_kernel", "1791809232494830.001", "0.002")
        b = task("main_kernel", "1791809232494830.002", "0.002")
        self.assertEqual(str(intersection(a, b)), "0.001")
        b["ts"] = "1791809232494830.003"
        self.assertEqual(intersection(a, b), 0)


if __name__ == "__main__":
    unittest.main()
