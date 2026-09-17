#!/usr/bin/env python3
"""完整 Dispatch 验证的配置和结果判据；真实设备调用由 C++ consumer 完成。"""

import copy
import math
import os
import unittest
from unittest import mock

import workflow
from test_workflow import TemporaryRoot


class ChainTests(TemporaryRoot):
    def setUp(self):
        super().setUp()
        os.environ.update(TILEXR_VALIDATION_CHECKPOINT=workflow.DISPATCH_CHECKPOINT,
                          TILEXR_VALIDATION_BUILD_MODE="production", TILEXR_COMM_ID="192.0.2.1:10177")
        os.environ.pop("TILEXR_DEMO_BARRIER_ADDR", None)
        self.hostfile.write_text("127.0.0.1\n192.0.2.2\n", encoding="utf-8")

    def test_runtime_defaults_include_actual_shape_and_endpoints(self):
        config = self.config()
        self.assertEqual(config.chain, dict(bs=96, h=7168, topk=6, moe_expert_num=96, dtype="bf16", quant_mode=0, fp8_format="e4m3",
                                           block_dim="auto", comm_id="192.0.2.1:10177", barrier_addr="192.0.2.1:10274",
                                           run_mode="functional", loops=1, warmup=0,
                                           route_mode="balance_shift", seed=2026, kernel_scope="dispatch", case_analysis=0, send_core_num=32, recv_core_num=32))
        original = config.chain.copy()
        for key, value in (("TILEXR_VALIDATION_BS", "1"), ("TILEXR_VALIDATION_DTYPE", "fp16"),
                           ("TILEXR_VALIDATION_BLOCK_DIM", "64"), ("TILEXR_COMM_ID", "192.0.2.1:20000"),
                           ("TILEXR_DEMO_BARRIER_ADDR", "192.0.2.1:20100")):
            with self.subTest(key=key), mock.patch.dict(os.environ, {key: value}):
                self.assertNotEqual(self.config().chain, original)

    def test_runtime_splits_and_remote_propagation(self):
        for send in (16, 24, 32, 40, 48):
            with self.subTest(send=send), mock.patch.dict(os.environ, TILEXR_VALIDATION_SEND_CORE_NUM=str(send)):
                config = self.config()
                self.assertEqual(config.chain['send_core_num'], send)
                self.assertEqual(config.chain['recv_core_num'], 64 - send)
                argv = workflow.remote_argv(config, '192.0.2.2', 'node', 'run', 'build')
                self.assertIn(f'TILEXR_VALIDATION_SEND_CORE_NUM={send}', argv[-1])

    def test_rejects_host_mode_bad_endpoints_and_unsupported_shapes(self):
        for key, values in {
            "TILEXR_VALIDATION_BUILD_MODE": ["host"],
            "TILEXR_COMM_ID": ["", "alias:123", "127.0.0.1:123", "0.0.0.0:123", "192.0.2.1:0", "192.0.2.1:65536"],
            "TILEXR_DEMO_BARRIER_ADDR": ["192.0.2.1:10177", "bad"],
            "TILEXR_VALIDATION_BS": ["0", "-1", str(1 << 63)],
            "TILEXR_VALIDATION_TOPK": ["33"],
            "TILEXR_VALIDATION_MOE_EXPERT_NUM": ["0", "33", "1040"],
            "TILEXR_VALIDATION_DTYPE": ["fp32"],
            "TILEXR_VALIDATION_QUANT_MODE": ["3", "1"],
            "TILEXR_VALIDATION_FP8_FORMAT": ["bad"],
            "TILEXR_VALIDATION_BLOCK_DIM": ["4", "0", "-1", "1.5", str(1 << 32)],
            "TILEXR_VALIDATION_SEND_CORE_NUM": ["", "0", "8", "17", "31", "33", "56", "64", "032", "32.0"],
            "TILEXR_VALIDATION_CASE_ANALYSIS": ["", "2", "01", "true"],
        }.items():
            for value in values:
                with self.subTest(key=key, value=value), mock.patch.dict(os.environ, {key: value}), \
                        self.assertRaises(ValueError):
                    self.config().hosts()

    def test_total_expert_limit_allows_one_and_more_than_32_local_experts(self):
        for ranks, experts in ((16, 16), (16, 528), (16, 1024), (24, 1008), (128, 1024)):
            with self.subTest(ranks=ranks, experts=experts), mock.patch.dict(os.environ, {
                    "TILEXR_VALIDATION_MOE_EXPERT_NUM": str(experts),
                    "TILEXR_VALIDATION_BS": str(experts // math.gcd(experts, ranks * 6))}):
                self.hostfile.write_text("\n".join(f"192.0.2.{i + 1}" for i in range(ranks // 8)) + "\n")
                config = self.config()
                self.assertEqual(len(config.hosts()) * 8, ranks)
                self.assertEqual(config.chain["moe_expert_num"], experts)

    def test_requires_both_current_markers_and_one_completed_launch(self):
        manifest = {"build_id": "build", "checkpoint": workflow.DISPATCH_CHECKPOINT, "source": {"commit": "a" * 40}}
        config = self.config()
        mappings = workflow.rank_mapping(self.config().hosts())[:8]
        records = []
        for mapping in mappings:
            identity = dict(workflow.rank_record(mapping, "run", manifest), pid=mapping["rank"] + 100, **config.chain)
            records.append(dict(identity, chain=dict(identity, peer_count=16, peer_mem_bytes=8 * 1024 ** 3),
                                submitted=1, completed=1, warmup_submitted=0, warmup_completed=0, cleanup="complete",
                                first_hop_packets=2, first_hop_received=2, first_hop_bytes=31744, token_stride=15872, direct_packets=3, direct_received=3, direct_bytes=47616,
                                local_data_bytes=15872, same_server_data_bytes=47616, cross_server_data_bytes=47616,
                                relay_packets=2, relay_received=2, relay_bytes=31744, data_bytes=111104,
                                relay_validated=True, dispatch_outputs_validated=True, group_validated=True, flags_cleared=True, prefix_indices_validated=True,
                                relay_links=[[0, token, 0, mapping["rank"], token, original_k, 1 if original_k == 0 else -1]
                                             for token in range(2) for original_k in range(2)],
                                first_hop_validated=True, direct_validated=True, route_coverage_validated=True, source_outputs_validated=True, weights_unchanged=True, native_status="dispatch"))
        workflow.validate_rank_records(records, mappings, "run", manifest, config.chain)
        for marker, key, value in ((None, "send_core_num", 48), ("chain", "recv_core_num", 16), (None, "completed", 0), (None, "submitted", 2),
                                   (None, "cleanup", "failed"), (None, "chain", None),
                                   ("chain", "peer_count", 8), ("chain", "peer_mem_bytes", 0),
                                   ("chain", "peer_mem_bytes", 12 * 1024 ** 2),
                                   ("chain", "peer_mem_bytes", 4 * 1024 ** 3),
                                   ("chain", "peer_mem_bytes", 8 * 1024 ** 3 + 32),
                                   ("chain", "pid", 777), ("chain", "run_id", "old-run"),
                                   ("chain", "checkpoint", workflow.DRY_RUN_CHECKPOINT)):
            changed = copy.deepcopy(records)
            target = changed[0][marker] if marker else changed[0]
            target[key] = value
            with self.subTest(marker=marker, key=key), self.assertRaises(workflow.Failure):
                workflow.validate_rank_records(changed, mappings, "run", manifest, config.chain)

    def test_dispatch_repeated_submissions_in_all_modes(self):
        for mode in ("functional", "performance", "breakdown"):
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_RUN_MODE": mode}):
                chain = self.config().chain
                self.assertEqual((chain["run_mode"], chain["loops"], chain["warmup"]), (mode, 1, 0))
                with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_LOOPS": "3", "TILEXR_VALIDATION_WARMUP": "2"}):
                    repeated = self.config().chain
                    self.assertEqual((repeated["loops"], repeated["warmup"]), (3, 2))
                for key, value in (("LOOPS", "0"), ("WARMUP", "-1")):
                    with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_" + key: value}), self.assertRaises(ValueError):
                        self.config()
        for checkpoint in ("ep-sdedup-p1-chain-single", "ep-sdedup-p1-chain-multi", "ep-sdedup-p2-first-hop", "ep-sdedup-p2-source-send"):
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_CHECKPOINT": checkpoint}), self.assertRaises(ValueError):
                self.config()
        for quant, format, stride in (("0", "e4m3", 15872), ("4", "e4m3", 8192), ("4", "e5m2", 8192)):
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_QUANT_MODE": quant, "TILEXR_VALIDATION_FP8_FORMAT": format}):
                self.assertEqual(workflow.source_send_stride(self.config().chain), stride)

    def test_multi_requires_all_configured_rounds_and_batch_identity(self):
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.DISPATCH_CHECKPOINT
        os.environ["TILEXR_VALIDATION_LOOPS"] = "1"
        config = self.config()
        manifest = {"build_id": "build", "checkpoint": config.checkpoint, "source": {"commit": "a" * 40}}
        mapping = workflow.rank_mapping(config.hosts())[0]
        identity = dict(workflow.rank_record(mapping, "run", manifest), pid=100,
                        **{key: config.chain[key] for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "run_mode", "loops", "warmup", "route_mode", "seed", "quant_mode", "fp8_format", "kernel_scope", "send_core_num", "recv_core_num")})
        record = dict(identity, chain=dict(identity, peer_count=16, peer_mem_bytes=8 * 1024 ** 3),
                      submitted=1, completed=1, warmup_submitted=0, warmup_completed=0, cleanup="complete",
                      first_hop_packets=2, first_hop_received=2, first_hop_bytes=31744, token_stride=15872, direct_packets=3, direct_received=3, direct_bytes=47616,
                                local_data_bytes=15872, same_server_data_bytes=47616, cross_server_data_bytes=47616,
                                relay_packets=2, relay_received=2, relay_bytes=31744, data_bytes=111104,
                                relay_validated=True, dispatch_outputs_validated=True, group_validated=True, flags_cleared=True, prefix_indices_validated=True,
                                relay_links=[[0, token, 0, mapping["rank"], token, original_k, 1 if original_k == 0 else -1]
                                             for token in range(2) for original_k in range(2)],
                      first_hop_validated=True, direct_validated=True, route_coverage_validated=True, source_outputs_validated=True, weights_unchanged=True, native_status="dispatch")
        workflow.validate_rank_records([record], [mapping], "run", manifest, config.chain)
        for key, value in (("first_hop_validated", False), ("token_stride", 8192), ("first_hop_bytes", 1),
                           ("direct_validated", False), ("relay_validated", False), ("dispatch_outputs_validated", False),
                           ("group_validated", False), ("flags_cleared", False), ("prefix_indices_validated", False),
                           ("relay_bytes", 0), ("relay_received", None), ("data_bytes", 79360), ("relay_links", []), ("route_coverage_validated", False), ("direct_bytes", 0),
                           ("direct_packets", True), ("direct_received", None), ("same_server_data_bytes", -1),
                           ("weights_unchanged", False), ("source_outputs_validated", False), ("direct_packets", 1),
                           ("quant_mode", 3), ("kernel_scope", "cnt_cumsum"), ("loops", 3), ("submitted", 4), ("completed", 4), ("completed", 6),
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
                        **{key: config.chain[key] for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "route_mode", "seed", "quant_mode", "fp8_format", "kernel_scope", "send_core_num", "recv_core_num")})
        record = dict(identity, chain=dict(identity, peer_count=16, peer_mem_bytes=4 * 1024 ** 3),
                      peer_mem_bytes=4 * 1024 ** 3, boundary_offset=4 * 1024 ** 3 - 32, probe_bytes=32,
                      submitted=1, completed=1, cleanup="complete", initial_zero=True, device_initial_zero=True,
                      device_readback_ff=True, host_boundary_ff=True, guard_zero=True)
        workflow.validate_rank_records([record], [mapping], "run", manifest, config.chain)
        changed = copy.deepcopy(record)
        changed["chain"]["peer_mem_bytes"] = 8 * 1024 ** 3
        with self.assertRaises(workflow.Failure):
            workflow.validate_rank_records([changed], [mapping], "run", manifest, config.chain)
        for key, value in (("peer_mem_bytes", 12 * 1024 ** 2), ("boundary_offset", 0), ("probe_bytes", 31),
                           ("initial_zero", False), ("device_initial_zero", False), ("device_readback_ff", False),
                           ("host_boundary_ff", False), ("guard_zero", False), ("completed", 0)):
            with self.subTest(key=key), self.assertRaises(workflow.Failure):
                workflow.validate_rank_records([dict(record, **{key: value})], [mapping], "run", manifest, config.chain)

    def test_dispatch_links_match_actual_relay_t_across_ranks_and_require_all_children(self):
        config = dict(self.config().chain, rank_size=16, bs=2, topk=3)
        capacity = 16 * config["bs"] * min(config["topk"], config["moe_expert_num"] // 16)
        records = []
        for rank in range(16):
            previous = rank // 8 * 8 + (rank + 7) % 8
            records.append(dict(rank=rank, first_hop_packets=1, first_hop_received=1,
                                relay_packets=1, relay_received=1, direct_packets=4,
                                relay_links=[[(rank + 8) % 16, 0, 1, rank, capacity - 1 - rank, 1, 1],
                                             [(previous + 8) % 16, 0, 1, previous, capacity - 1 - previous, 2, -1]]))
        workflow.validate_dispatch_links(records, config)
        for fault in ("missing", "shape", "type", "owner", "range", "count", "terminal-count", "t", "orphan", "duplicate", "reuse", "routes"):
            changed = copy.deepcopy(records)
            first, terminal = changed[0]["relay_links"]
            if fault == "missing": changed[0].pop("relay_links")
            if fault == "shape": first.pop()
            if fault == "type": first[4] = True
            if fault == "owner": first[3] = 1
            if fault == "range": first[4] = capacity
            if fault == "count": first[6] = 2
            if fault == "terminal-count": terminal[6] = 0
            if fault == "t": terminal[4] += 1
            if fault == "orphan": terminal[1] = 1
            if fault == "duplicate": changed[0]["relay_links"].append(terminal[:])
            if fault == "reuse":
                changed[0]["relay_links"].append([8, 1, 1, 0, capacity - 1, 1, 1])
                changed[0]["first_hop_received"] += 1
            if fault == "routes": changed[0]["direct_packets"] -= 1
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                workflow.validate_dispatch_links(changed, config)

    def test_relay_prefix_capacity_uses_full_output_rows(self):
        for experts in (16, 32, 96):
            config = dict(bs=2, topk=3, moe_expert_num=experts)
            capacity = 16 * 2 * min(3, experts // 16)
            for index in (16, capacity - 1):
                record = dict(rank=8, first_hop_received=1, relay_received=0,
                              relay_links=[[0, 0, 0, 8, index, 0, 1]])
                workflow.validate_relay_links(record, config, 16)
            for index in (-1, capacity):
                record["relay_links"][0][4] = index
                with self.assertRaises(ValueError):
                    workflow.validate_relay_links(record, config, 16)

    def test_memory_boundary_is_one_functional_case(self):
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.MEMORY_BOUNDARY_CHECKPOINT
        self.assertEqual(self.config().chain["run_mode"], "functional")
        for key, value in (("RUN_MODE", "performance"), ("RUN_MODE", "breakdown"), ("LOOPS", "2"), ("WARMUP", "1")):
            with self.subTest(key=key, value=value), mock.patch.dict(os.environ, {"TILEXR_VALIDATION_" + key: value}), \
                    self.assertRaises(ValueError):
                self.config()


if __name__ == "__main__":
    unittest.main()
