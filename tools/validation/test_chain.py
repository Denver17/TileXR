#!/usr/bin/env python3
"""单轮空 launch 的配置和结果判据；真实设备调用由 C++ consumer 完成。"""

import copy
import os
import unittest
from unittest import mock

import workflow
from test_workflow import TemporaryRoot


class ChainTests(TemporaryRoot):
    def setUp(self):
        super().setUp()
        os.environ.update(TILEXR_VALIDATION_CHECKPOINT=workflow.CHAIN_CHECKPOINT,
                          TILEXR_VALIDATION_BUILD_MODE="production", TILEXR_COMM_ID="192.0.2.1:10177")
        os.environ.pop("TILEXR_DEMO_BARRIER_ADDR", None)
        self.hostfile.write_text("localhost\nnode-b\n", encoding="utf-8")

    def test_defaults_and_signature_include_actual_shape_and_endpoints(self):
        config = self.config()
        self.assertEqual(config.chain, dict(bs=256, h=7168, topk=6, moe_expert_num=384, dtype="bf16",
                                           block_dim="auto", comm_id="192.0.2.1:10177", barrier_addr="192.0.2.1:10274"))
        original = config.signature()
        for key, value in (("TILEXR_VALIDATION_BS", "1"), ("TILEXR_VALIDATION_DTYPE", "fp16"),
                           ("TILEXR_VALIDATION_BLOCK_DIM", "1"), ("TILEXR_COMM_ID", "192.0.2.1:20000"),
                           ("TILEXR_DEMO_BARRIER_ADDR", "192.0.2.1:20100")):
            with self.subTest(key=key), mock.patch.dict(os.environ, {key: value}):
                self.assertNotEqual(self.config().signature(), original)

    def test_rejects_host_mode_bad_endpoints_and_unsupported_shapes(self):
        for key, values in {
            "TILEXR_VALIDATION_BUILD_MODE": ["host"],
            "TILEXR_COMM_ID": ["", "alias:123", "127.0.0.1:123", "0.0.0.0:123", "192.0.2.1:0", "192.0.2.1:65536"],
            "TILEXR_DEMO_BARRIER_ADDR": ["192.0.2.1:10177", "bad"],
            "TILEXR_VALIDATION_BS": ["0", "-1", str(1 << 63)],
            "TILEXR_VALIDATION_TOPK": ["33"],
            "TILEXR_VALIDATION_MOE_EXPERT_NUM": ["16", "33", "528"],
            "TILEXR_VALIDATION_DTYPE": ["fp32"],
            "TILEXR_VALIDATION_BLOCK_DIM": ["0", "-1", "1.5", str(1 << 32)],
        }.items():
            for value in values:
                with self.subTest(key=key, value=value), mock.patch.dict(os.environ, {key: value}), \
                        self.assertRaises(ValueError):
                    self.config().signature()

    def test_requires_both_current_markers_and_one_completed_launch(self):
        manifest = {"build_id": "build", "checkpoint": workflow.CHAIN_CHECKPOINT, "source": {"commit": "a" * 40}}
        mappings = workflow.rank_mapping(self.config().hosts())[:8]
        records = []
        for mapping in mappings:
            identity = dict(workflow.rank_record(mapping, "run", manifest), pid=mapping["rank"] + 100)
            records.append(dict(identity, chain=dict(identity, peer_count=16, peer_mem_bytes=1048576),
                                submitted=1, completed=1, cleanup="complete"))
        workflow.validate_rank_records(records, mappings, "run", manifest)
        for marker, key, value in ((None, "completed", 0), (None, "submitted", 2),
                                   (None, "cleanup", "failed"), (None, "chain", None),
                                   ("chain", "peer_count", 8), ("chain", "peer_mem_bytes", 0),
                                   ("chain", "pid", 777), ("chain", "run_id", "old-run"),
                                   ("chain", "checkpoint", workflow.DRY_RUN_CHECKPOINT)):
            changed = copy.deepcopy(records)
            target = changed[0][marker] if marker else changed[0]
            target[key] = value
            with self.subTest(marker=marker, key=key), self.assertRaises(workflow.Failure):
                workflow.validate_rank_records(changed, mappings, "run", manifest)


if __name__ == "__main__":
    unittest.main()
