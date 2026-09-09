#!/usr/bin/env python3
"""dry-run 映射和真实子进程回归；不访问 NPU 或真实 SSH。"""

import copy
import os
import subprocess
import sys
import threading
import time
from unittest import mock
import unittest

import workflow
from test_workflow import TemporaryRoot


class DryRunTests(TemporaryRoot):
    def setUp(self):
        super().setUp()
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.DRY_RUN_CHECKPOINT
        self.hostfile.write_text("localhost\nnode-b\n", encoding="utf-8")
        self.manifest = {"build_id": "dry-build", "checkpoint": workflow.DRY_RUN_CHECKPOINT,
                         "mode": "host", "source": {"commit": "a" * 40}, "tests": []}
        workflow.write_json(self.config().install / "manifest.json", self.manifest)

    def test_mapping_preserves_host_and_physical_device_order_at_64p_and_128p(self):
        devices = [7, 2, 5, 0, 6, 1, 4, 3]
        for count in (8, 16):
            self.hostfile.write_text("\n".join(f"node-{index} devices=" + ",".join(map(str, devices))
                                               for index in range(count)), encoding="utf-8")
            mappings = workflow.rank_mapping(self.config().hosts())
            self.assertEqual([item["rank"] for item in mappings], list(range(count * 8)))
            for rank, item in enumerate(mappings):
                self.assertEqual(item, {"host": f"node-{rank // 8}", "host_index": rank // 8,
                                        "local_rank": rank % 8, "device": devices[rank % 8],
                                        "rank": rank, "rank_size": count * 8})

    def test_rejects_unsupported_topology(self):
        for hosts in ("localhost\n", "localhost devices=0,1\nnode-b\n",
                      "\n".join(f"node-{i}" for i in range(17)),
                      "localhost\nlocalhost\n", "localhost devices=0,1,2,3,4,5,6,6\nnode-b\n"):
            with self.subTest(hosts=hosts), self.assertRaises(ValueError):
                self.hostfile.write_text(hosts, encoding="utf-8")
                self.config().hosts()

    def test_controller_rejects_incomplete_stale_duplicate_or_malformed_rank_results(self):
        expected = workflow.rank_mapping(self.config().hosts())[:8]
        records = [dict(workflow.rank_record(item, "run", self.manifest), pid=100 + item["rank"])
                   for item in expected]
        result = dict(run_id="run", build_id="dry-build", checkpoint=workflow.DRY_RUN_CHECKPOINT,
                      mode="host", status="PASS", cleanup="complete", tests=[], ranks=records)
        workflow.validate_result(result, "run", self.manifest, expected)
        mutations = [None, [], records[:-1], [records[0]] * 8, [None] * 8]
        for key, value in (("rank", 8), ("rank", False), ("run_id", "old"), ("build_id", "old"),
                           ("commit", "b" * 40), ("checkpoint", workflow.CHECKPOINT),
                           ("host", "foreign"), ("host_index", 1), ("local_rank", 7),
                           ("device", 7), ("rank_size", 128), ("status", "FAIL"), ("pid", 0)):
            changed = copy.deepcopy(records)
            changed[0][key] = value
            mutations.append(changed)
        for changed in mutations:
            with self.subTest(records=changed), self.assertRaises(workflow.Failure):
                workflow.validate_result(dict(result, ranks=changed), "run", self.manifest, expected)
        with self.assertRaises(workflow.Failure):
            workflow.validate_result(result, "run", self.manifest)

    def test_rank_worker_rejects_foreign_build_and_out_of_range_rank(self):
        for build_id, rank in (("old-build", "0"), ("dry-build", "16"), ("dry-build", "-1")):
            result = subprocess.run([sys.executable, workflow.__file__, "rank", "run", build_id, rank],
                                    capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(workflow.DRY_RUN_MARKER, result.stdout)

    @unittest.skipUnless(os.name == "posix", "进程组契约需要 POSIX Python")
    def test_real_rank_processes_cover_64p_and_128p_mapping(self):
        # 本机逐节点运行真实 rank 进程；这是编排回归，不是目标集群验收。
        for hosts in (8, 16):
            self.hostfile.write_text("\n".join(f"node-{i}" for i in range(hosts)), encoding="utf-8")
            config = self.config()
            mappings = workflow.rank_mapping(config.hosts())
            records = []
            deadline = time.monotonic() + 120
            for index in range(hosts):
                result = {"errors": []}
                workflow.run_ranks(config, f"local-{hosts * 8}", self.manifest,
                    mappings[index * 8:(index + 1) * 8], deadline, threading.Event(), result)
                self.assertEqual(result["errors"], [])
                self.assertEqual(len({record["pid"] for record in result["ranks"]}), 8)
                records.extend(result["ranks"])
            workflow.validate_rank_records(records, mappings, f"local-{hosts * 8}", self.manifest)

    def test_expired_deadline_does_not_start_rank_or_grant_new_budget(self):
        result = {"errors": []}
        with mock.patch.object(workflow, "execute") as execute:
            workflow.run_ranks(self.config(), "expired", self.manifest,
                workflow.rank_mapping(self.config().hosts())[:8], time.monotonic() - 1,
                threading.Event(), result)
        execute.assert_not_called()
        self.assertTrue(result["errors"][0]["timeout"])
        self.assertEqual(result["ranks"], [])


if __name__ == "__main__":
    unittest.main()
