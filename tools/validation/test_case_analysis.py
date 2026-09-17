"""Route-volume analysis against retained inputs; no NPU needed."""

from pathlib import Path
import struct
import tempfile
import unittest

import case_analysis


class CaseAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = dict(rank_size=24, moe_expert_num=48, bs=2, topk=4)

    def save(self, path, ids):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(struct.pack(f"<{len(ids)}i", *ids))
        return path

    def test_groups_per_token_and_remote_server_not_per_rank(self):
        # Rank 0: server 0 experts [0,16) are local, even on another card.
        path = self.save(self.root / "ids.bin", [14, 16, 17, 32, 1, 2, 33, 34])
        row = case_analysis.analyze_rank(path, 0, self.config)
        self.assertEqual((row["before_tokens"], row["after_tokens"]), (5, 3))
        self.assertEqual(row["dedup_percent"], 40.0)
        self.assertEqual(len(row["expert_ids_sha256"]), 64)
        # The same list has a different local server on source rank 8.
        row = case_analysis.analyze_rank(path, 8, self.config)
        self.assertEqual((row["before_tokens"], row["after_tokens"]), (6, 4))

    def test_different_tokens_are_never_merged(self):
        path = self.save(self.root / "ids.bin", [16, 17, 18, 19] * 2)
        row = case_analysis.analyze_rank(path, 0, self.config)
        self.assertEqual((row["before_tokens"], row["after_tokens"]), (8, 2))
        self.assertEqual(row["dedup_percent"], 75.0)

    def test_no_savings_and_no_cross_server_traffic(self):
        for ids, before in (([0, 1, 16, 32] * 2, 4), ([0, 1, 2, 15] * 2, 0)):
            row = case_analysis.analyze_rank(self.save(self.root / "ids.bin", ids), 0, self.config)
            self.assertEqual((row["before_tokens"], row["after_tokens"]), (before, before))
            self.assertEqual(row["dedup_percent"], 0.0)

    def test_missing_truncated_and_invalid_expert_inputs_fail(self):
        path = self.root / "ids.bin"
        with self.assertRaises(FileNotFoundError):
            case_analysis.analyze_rank(path, 0, self.config)
        for ids in ([0], [0] * 9, [-1] * 8, [48] * 8):
            self.save(path, ids)
            with self.assertRaises(ValueError):
                case_analysis.analyze_rank(path, 0, self.config)

    def test_all_ranks_summary_uses_unweighted_rank_average(self):
        config = dict(rank_size=16, moe_expert_num=32, bs=1, topk=4)
        for rank in range(16):
            ids = [rank // 8 * 16 + i for i in range(4)]
            if rank == 0:
                ids = [0, 1, 2, 16]  # A=1, B=1; 0%.
            elif rank == 1:
                ids = [16, 17, 18, 19]  # A=4, B=1; 75%.
            node = self.root if rank < 8 else self.root / "nodes" / "1"
            self.save(node / "inputs" / f"rank-{rank}-ids.bin", ids)
        records = [dict(rank=rank) for rank in range(16)]
        report = case_analysis.analyze_case(self.root, config, records)
        self.assertEqual((report["min"], report["max"], report["avg"]), (0.0, 75.0, 75.0 / 16))
        self.assertEqual(report["zero_cross_server_ranks"], 14)
        self.assertEqual(report["phase"], "after_npu")
        self.assertEqual(len(report["ranks"]), 16)
        for incomplete in (records[:-1], records[:-1] + [records[0]]):
            with self.assertRaisesRegex(ValueError, "every rank"):
                case_analysis.analyze_case(self.root, config, incomplete)


if __name__ == "__main__":
    unittest.main()
