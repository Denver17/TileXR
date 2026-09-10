"""Trace serialization and Memory-style rank selection, without NPU execution."""
import json
from pathlib import Path
import tempfile
import unittest
import measurements


class TraceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name) / "traces"
        self.config = dict(run_mode="breakdown", loops=5, warmup=2, rank_size=4,
                           bs=256, h=7168, topk=6, moe_expert_num=384, dtype="bf16", block_dim="auto")
        self.records = []
        for rank, duration in enumerate((9000, 1000, 4000, 4000)):
            origin = 100000000000000000 + rank * 999999999
            self.records.append(dict(rank=rank, host=f"host-{rank}", device=rank,
                breakdown=dict(generation=7, run_mode="breakdown", loops=5, warmup=2, block_dim=2,
                    records=[dict(core=core, start=origin + core * 100,
                                  checked=origin + core * 100 + 200,
                                  end=origin + core * 100 + duration) for core in range(2)])))

    def export(self):
        return measurements.export_breakdown_traces(self.records, self.config, self.output, "run-test", "a" * 64)

    def test_every_rank_and_core_exported_with_normalized_microseconds_and_selection(self):
        result = self.export()
        self.assertEqual(result["selected_ranks"], dict(fastest=1, p50=2, slowest=0))
        self.assertEqual(len(result["ranks"]), 4)
        combined = json.loads(Path(result["all_ranks"]).read_text())
        events = [e for e in combined["traceEvents"] if e["ph"] == "X"]
        self.assertEqual(len(events), 16)
        self.assertEqual({e["pid"] for e in events}, set(range(4)))
        for rank, path in result["ranks"].items():
            trace = json.loads(Path(path).read_text())
            slices = [e for e in trace["traceEvents"] if e["ph"] == "X"]
            self.assertEqual({e["pid"] for e in slices}, {int(rank)})
            self.assertEqual({e["tid"] for e in slices}, {0, 1})
            self.assertEqual(min(e["ts"] for e in slices), 0)
            self.assertEqual(slices[0]["dur"], 0.2)
        for role, path in result["selected"].items():
            trace = json.loads(Path(path).read_text())
            self.assertEqual(trace["otherData"]["rank"], result["selected_ranks"][role])
        self.assertEqual(json.loads((self.output / "index.json").read_text()), result)

    def test_incomplete_rank_or_stale_generation_rejected_before_writing(self):
        self.records.pop()
        with self.assertRaises(ValueError):
            self.export()
        self.assertFalse(self.output.exists())
        self.config["rank_size"] = 3
        self.records[0]["breakdown"]["generation"] = 6
        with self.assertRaises(ValueError):
            self.export()
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
