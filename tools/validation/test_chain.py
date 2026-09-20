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
        self.assertEqual(config.chain, dict(bs=96, h=7168, topk=6, moe_expert_num=96, dtype="bf16", quant_mode=0, operator_mode="dispatch", dispatch_quant_mode=0, combine_quant_mode=0, fp8_format="e4m3",
                                           block_dim="auto", comm_id="192.0.2.1:10177", barrier_addr="192.0.2.1:10274",
                                           run_mode="functional", loops=1, warmup=0,
                                           route_mode="balance_shift", seed=2026, kernel_scope="dispatch", case_analysis=0))
        original = config.chain.copy()
        for key, value in (("TILEXR_VALIDATION_BS", "1"), ("TILEXR_VALIDATION_DTYPE", "fp16"),
                           ("TILEXR_VALIDATION_BLOCK_DIM", "64"), ("TILEXR_COMM_ID", "192.0.2.1:20000"),
                           ("TILEXR_DEMO_BARRIER_ADDR", "192.0.2.1:20100")):
            with self.subTest(key=key), mock.patch.dict(os.environ, {key: value}):
                self.assertNotEqual(self.config().chain, original)

    def test_server_dedup_is_compile_time_and_not_exported(self):
        config = self.config()
        self.assertNotIn("server_dedup_enabled", config.chain)
        argv = workflow.remote_argv(config, "192.0.2.2", "node", "run", "build")
        self.assertNotIn("TILEXR_EP_SERVER_DEDUP_ENABLE", argv[-1])

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
            "TILEXR_VALIDATION_CASE_ANALYSIS": ["", "2", "01", "true"],
            "TILEXR_VALIDATION_ROUTE_MODE": ["", "uniform", "balance shift"],
            "TILEXR_VALIDATION_SEED": ["", "-1", "4294967296", "1.5", "+1", "01", " 1", "١"],
        }.items():
            for value in values:
                with self.subTest(key=key, value=value), mock.patch.dict(os.environ, {key: value}), \
                        self.assertRaises(ValueError):
                    self.config().hosts()

    def test_route_modes_preserve_expert_mapping_and_shift_epoch_constraint(self):
        # 16 tokens do not fill the 64-token shift epoch; other generators allow it.
        for mode in ("random", "balance", "balance_shift"):
            with self.subTest(mode=mode), mock.patch.dict(os.environ, {
                    "TILEXR_VALIDATION_ROUTE_MODE": mode, "TILEXR_VALIDATION_BS": "1",
                    "TILEXR_VALIDATION_MOE_EXPERT_NUM": "384", "TILEXR_VALIDATION_SEED": "4294967295"}):
                config = self.config()
                self.assertEqual((config.chain["route_mode"], config.chain["seed"]), (mode, 4294967295))
                if mode == "balance_shift":
                    with self.assertRaisesRegex(ValueError, "balance_shift"):
                        config.hosts()
                else:
                    self.assertEqual(len(config.hosts()), 2)
                with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_MOE_EXPERT_NUM": "383"}):
                    with self.assertRaisesRegex(ValueError, "rank_size"):
                        self.config().hosts()

    def test_requested_128p_shape_supports_all_route_modes_and_seed_boundaries(self):
        self.hostfile.write_text("\n".join(f"192.0.2.{i + 1}" for i in range(16)) + "\n")
        for mode in ("random", "balance", "balance_shift"):
            for seed in (0, 2026, 4294967295):
                with self.subTest(mode=mode, seed=seed), mock.patch.dict(os.environ, {
                        "TILEXR_VALIDATION_ROUTE_MODE": mode, "TILEXR_VALIDATION_SEED": str(seed),
                        "TILEXR_VALIDATION_MOE_EXPERT_NUM": "384", "TILEXR_VALIDATION_BS": "256"}):
                    config = self.config()
                    self.assertEqual(len(config.hosts()) * 8, 128)
                    self.assertEqual(config.chain["moe_expert_num"] // 128, 3)
                    self.assertEqual(config.chain["seed"], seed)

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
            identity = dict(workflow.rank_record(mapping, "run", manifest), pid=mapping["rank"] + 100, server_dedup_enabled=True, **config.chain)
            records.append(dict(identity, chain=dict(identity, peer_count=16, peer_mem_bytes=8 * 1024 ** 3),
                                submitted=1, completed=1, warmup_submitted=0, warmup_completed=0, cleanup="complete",
                                first_hop_packets=2, first_hop_received=2, first_hop_bytes=31744, token_stride=15872, direct_packets=3, direct_received=3, direct_bytes=47616,
                                local_data_bytes=15872, same_server_data_bytes=47616, cross_server_data_bytes=47616,
                                relay_packets=2, relay_received=2, relay_bytes=31744, data_bytes=111104,
                                relay_validated=True, dispatch_outputs_validated=True, five_tables_validated=True, member_addresses_validated=True, flags_cleared=True,
                                first_hop_validated=True, direct_validated=True, route_coverage_validated=True, source_outputs_validated=True, weights_unchanged=True, native_status="dispatch"))
        workflow.validate_rank_records(records, mappings, "run", manifest, config.chain)
        for marker, key, value in ((None, "completed", 0), (None, "submitted", 2),
                                   (None, "route_mode", "random"), ("chain", "route_mode", "balance"),
                                   (None, "seed", 7), ("chain", "seed", 0),
                                   (None, "server_dedup_enabled", False), ("chain", "server_dedup_enabled", False),
                                   (None, "server_dedup_enabled", None), (None, "server_dedup_enabled", 1),
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

    def test_packet_stride_keeps_entire_index_inside_one_data_block(self):
        for h, quant, k, expected in ((208, 0, 1, 512), (208, 0, 4, 512),
                                     (208, 0, 5, 1024), (256, 4, 20, 512),
                                     (256, 4, 21, 1024), (256, 4, 32, 1024)):
            with self.subTest(h=h, quant=quant, k=k):
                self.assertEqual(workflow.source_send_stride(dict(h=h, quant_mode=quant, topk=k)), expected)

    def test_multi_requires_all_configured_rounds_and_batch_identity(self):
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.DISPATCH_CHECKPOINT
        os.environ["TILEXR_VALIDATION_LOOPS"] = "1"
        config = self.config()
        manifest = {"build_id": "build", "checkpoint": config.checkpoint, "source": {"commit": "a" * 40}}
        mapping = workflow.rank_mapping(config.hosts())[0]
        identity = dict(workflow.rank_record(mapping, "run", manifest), pid=100, server_dedup_enabled=True,
                        **{key: config.chain[key] for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "run_mode", "loops", "warmup", "route_mode", "seed", "quant_mode", "fp8_format", "kernel_scope")})
        record = dict(identity, chain=dict(identity, peer_count=16, peer_mem_bytes=8 * 1024 ** 3),
                      submitted=1, completed=1, warmup_submitted=0, warmup_completed=0, cleanup="complete",
                      first_hop_packets=2, first_hop_received=2, first_hop_bytes=31744, token_stride=15872, direct_packets=3, direct_received=3, direct_bytes=47616,
                                local_data_bytes=15872, same_server_data_bytes=47616, cross_server_data_bytes=47616,
                                relay_packets=2, relay_received=2, relay_bytes=31744, data_bytes=111104,
                                relay_validated=True, dispatch_outputs_validated=True, five_tables_validated=True, member_addresses_validated=True, flags_cleared=True,
                      first_hop_validated=True, direct_validated=True, route_coverage_validated=True, source_outputs_validated=True, weights_unchanged=True, native_status="dispatch")
        workflow.validate_rank_records([record], [mapping], "run", manifest, config.chain)
        for key, value in (("first_hop_validated", False), ("token_stride", 8192), ("first_hop_bytes", 1),
                           ("direct_validated", False), ("relay_validated", False), ("dispatch_outputs_validated", False),
                           ("five_tables_validated", False), ("flags_cleared", False), ("member_addresses_validated", False),
                           ("relay_bytes", 0), ("relay_received", None), ("data_bytes", 79360), ("route_coverage_validated", False), ("direct_bytes", 0),
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
        identity = dict(workflow.rank_record(mapping, "run", manifest), pid=100, server_dedup_enabled=True,
                        **{key: config.chain[key] for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "route_mode", "seed", "quant_mode", "fp8_format", "kernel_scope")})
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

    def test_dispatch_tables_require_all_ranks_matching_one_case(self):
        config = dict(self.config().chain, rank_size=16, bs=2, topk=3)
        records = [dict(config, rank=rank, run_id="run", build_id="build", commit="a" * 40,
                        server_dedup_enabled=True, dispatch_outputs_validated=True,
                        five_tables_validated=True, member_addresses_validated=True,
                        first_hop_packets=1, relay_packets=1, direct_packets=4) for rank in range(16)]
        workflow.validate_dispatch_tables(records, config)
        for fault in ("missing-rank", "duplicate-rank", "missing-table-verdict", "failed-address-verdict",
                      "different-seed", "different-build", "different-run", "missing-identity", "routes"):
            changed = copy.deepcopy(records)
            if fault == "missing-rank": changed.pop()
            if fault == "duplicate-rank": changed[0]["rank"] = 1
            if fault == "missing-table-verdict": changed[0].pop("five_tables_validated")
            if fault == "failed-address-verdict": changed[0]["member_addresses_validated"] = False
            if fault == "different-seed": changed[0]["seed"] += 1
            if fault == "different-build": changed[0]["build_id"] = "stale"
            if fault == "different-run": changed[0]["run_id"] = "stale"
            if fault == "missing-identity": changed[0].pop("seed")
            if fault == "routes": changed[0]["direct_packets"] -= 1
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                workflow.validate_dispatch_tables(changed, config)

    def test_memory_boundary_is_one_functional_case(self):
        os.environ["TILEXR_VALIDATION_CHECKPOINT"] = workflow.MEMORY_BOUNDARY_CHECKPOINT
        self.assertEqual(self.config().chain["run_mode"], "functional")
        for key, value in (("RUN_MODE", "performance"), ("RUN_MODE", "breakdown"), ("LOOPS", "2"), ("WARMUP", "1")):
            with self.subTest(key=key, value=value), mock.patch.dict(os.environ, {"TILEXR_VALIDATION_" + key: value}), \
                    self.assertRaises(ValueError):
                self.config()


if __name__ == "__main__":
    unittest.main()
