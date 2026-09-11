#!/usr/bin/env python3
"""单轮/多轮空 launch 的配置和结果判据；真实设备调用由 C++ consumer 完成。"""

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
                                           block_dim="auto", comm_id="192.0.2.1:10177", barrier_addr="192.0.2.1:10274",
                                           run_mode="functional", loops=1, warmup=0))
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

    def test_multi_defaults_loop_bounds_and_signature(self):
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.CHAIN_MULTI_CHECKPOINT
        original = self.config().signature()
        self.assertEqual(original["chain"]["loops"], 1)
        self.assertEqual(original["chain"]["run_mode"], "functional")
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_LOOPS": "3"}):
            self.assertEqual(self.config().chain["loops"], 3)
            self.assertNotEqual(self.config().signature(), original)
        for value in ("0", "-1", "1.5", str(1 << 32)):
            with self.subTest(value=value), mock.patch.dict(os.environ, {"TILEXR_VALIDATION_LOOPS": value}), \
                    self.assertRaises(ValueError):
                self.config()
        for mode in ("performance", "breakdown"):
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_RUN_MODE": mode}):
                chain = self.config().chain
                self.assertEqual((chain["run_mode"], chain["loops"], chain["warmup"]), (mode, 50, 50))
        for values in ({"TILEXR_VALIDATION_RUN_MODE": "bad"}, {"TILEXR_VALIDATION_WARMUP": "1"},
                       {"TILEXR_VALIDATION_RUN_MODE": "performance", "TILEXR_VALIDATION_WARMUP": "-1"}):
            with mock.patch.dict(os.environ, values), self.assertRaises(ValueError):
                self.config()
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_BUILD_MODE": "host"}), self.assertRaises(ValueError):
            self.config()
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.CHAIN_CHECKPOINT
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_LOOPS": "5"}):
            self.assertEqual(self.config().chain["loops"], 1)

    def test_multi_requires_all_configured_rounds_and_batch_identity(self):
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.CHAIN_MULTI_CHECKPOINT
        os.environ["TILEXR_VALIDATION_LOOPS"] = "5"
        config = self.config()
        manifest = {"build_id": "build", "checkpoint": config.checkpoint, "source": {"commit": "a" * 40}}
        mapping = workflow.rank_mapping(config.hosts())[0]
        identity = dict(workflow.rank_record(mapping, "run", manifest), pid=100,
                        **{key: config.chain[key] for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "run_mode", "loops", "warmup")})
        record = dict(identity, chain=dict(identity, peer_count=16, peer_mem_bytes=12 * 1024 * 1024),
                      submitted=5, completed=5, warmup_submitted=0, warmup_completed=0, cleanup="complete")
        workflow.validate_rank_records([record], [mapping], "run", manifest, config.chain)
        for key, value in (("loops", 3), ("submitted", 4), ("completed", 4), ("completed", 6),
                           ("completed", True), ("warmup_submitted", 1), ("warmup_completed", None),
                           ("run_mode", "performance"), ("cleanup", "failed"),
                           ("checkpoint", workflow.CHAIN_CHECKPOINT)):
            with self.subTest(key=key, value=value), self.assertRaises(workflow.Failure):
                workflow.validate_rank_records([dict(record, **{key: value})], [mapping], "run", manifest, config.chain)
        with self.assertRaises(workflow.Failure):
            workflow.validate_rank_records([record], [mapping], "run", manifest)

    def test_memory_boundary_requires_capacity_and_each_read_write_assertion(self):
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.MEMORY_BOUNDARY_CHECKPOINT
        config = self.config()
        manifest = {"build_id": "build", "checkpoint": config.checkpoint, "source": {"commit": "a" * 40}}
        mapping = workflow.rank_mapping(config.hosts())[0]
        identity = dict(workflow.rank_record(mapping, "run", manifest), pid=100,
                        **{key: config.chain[key] for key in ("bs", "h", "topk", "moe_expert_num", "dtype")})
        record = dict(identity, chain=dict(identity, peer_count=16, peer_mem_bytes=4 * 1024 ** 3),
                      peer_mem_bytes=4 * 1024 ** 3, boundary_offset=4 * 1024 ** 3 - 32, probe_bytes=32,
                      submitted=1, completed=1, cleanup="complete", initial_zero=True, device_initial_zero=True,
                      device_readback_ff=True, host_boundary_ff=True, guard_zero=True)
        workflow.validate_rank_records([record], [mapping], "run", manifest, config.chain)
        for key, value in (("peer_mem_bytes", 12 * 1024 ** 2), ("boundary_offset", 0), ("probe_bytes", 31),
                           ("initial_zero", False), ("device_initial_zero", False), ("device_readback_ff", False),
                           ("host_boundary_ff", False), ("guard_zero", False), ("completed", 0)):
            with self.subTest(key=key), self.assertRaises(workflow.Failure):
                workflow.validate_rank_records([dict(record, **{key: value})], [mapping], "run", manifest, config.chain)

    def test_memory_boundary_is_one_functional_case(self):
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.MEMORY_BOUNDARY_CHECKPOINT
        self.assertEqual(self.config().chain["run_mode"], "functional")
        for key, value in (("RUN_MODE", "performance"), ("RUN_MODE", "breakdown"), ("LOOPS", "2"), ("WARMUP", "1")):
            with self.subTest(key=key, value=value), mock.patch.dict(os.environ, {"TILEXR_VALIDATION_" + key: value}), \
                    self.assertRaises(ValueError):
                self.config()


if __name__ == "__main__":
    unittest.main()
