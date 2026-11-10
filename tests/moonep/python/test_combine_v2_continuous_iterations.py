#!/usr/bin/env python3
# 验证 MoonEP 的布局、调用或诊断合同；直接覆盖对应 src/moonep 与 tools/moonep 接口，硬件数据面另行验收。

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
PROBE = REPO_ROOT / "tests" / "moonep_combine_v2" / "demo" / \
    "tilexr_moonep_combine_v2_hardware_probe.cpp"
LAUNCHER = REPO_ROOT / "tools" / "moonep" / \
    "run_combine_v2_perf_multihost.sh"
CLUSTER_LAUNCHER = REPO_ROOT / "tools" / "moonep" / \
    "run_combine_v2_perf_cluster.sh"

class CombineV2ContinuousIterationsTest(unittest.TestCase):
    def test_timed_launches_have_no_inter_iteration_work(self):
        source = PROBE.read_text(encoding="utf-8-sig")
        begin = source.index('CheckAcl(rank, "aclrtRecordEvent batch start"')
        end = source.index("std::cout << std::fixed << std::setprecision(6)", begin)
        timed_batch = source[begin:end]

        loop_begin = timed_batch.index(
            "for (int iteration = 0; iteration < options.iterations; "
            "++iteration) {")
        loop_end = timed_batch.index("\n        }", loop_begin)
        loop = timed_batch[loop_begin:loop_end]

        self.assertEqual(loop.count("LaunchCombine("), 1)
        for forbidden in (
                "BarrierAll(", "aclrtSynchronize", "aclrtMemcpy",
                "aclrtRecordEvent", "std::cout"):
            self.assertNotIn(forbidden, loop)
        self.assertEqual(timed_batch.count("aclrtRecordEvent("), 2)
        self.assertEqual(timed_batch.count("aclrtSynchronizeEvent("), 1)
        self.assertIn("batchElapsedMs /", timed_batch)

    def test_launcher_aggregates_rank_batch_average(self):
        launcher = LAUNCHER.read_text(encoding="utf-8")

        self.assertIn('item[1] == "avg_ms"', launcher)
        self.assertIn("rank_average[rank_key]", launcher)
        self.assertNotIn("sample_count[rank_key] != iterations", launcher)

    def test_launchers_do_not_repeat_npu_preflight(self):
        launchers = (
            LAUNCHER.read_text(encoding="utf-8"),
            CLUSTER_LAUNCHER.read_text(encoding="utf-8"),
        )
        for launcher in launchers:
            self.assertNotIn("npu-smi", launcher)
            self.assertNotIn("NPU preflight", launcher)
            self.assertNotIn("--skip-npu-preflight", launcher)
            self.assertNotIn("--wait-seconds", launcher)
            self.assertNotIn("--retry-seconds", launcher)

if __name__ == "__main__":
    unittest.main()
