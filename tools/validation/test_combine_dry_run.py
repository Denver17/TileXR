#!/usr/bin/env python3
"""C3 configuration and local controller contracts; never SSH or devices."""

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import workflow


class CombineDryRunConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="combine-c3-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        environment = {key: value for key, value in os.environ.items() if not key.startswith("TILEXR_")}
        environment.update(TILEXR_VALIDATION_ROOT=str(self.root), TILEXR_VALIDATION_BUILD_MODE="host",
                           TILEXR_VALIDATION_CHECKPOINT=workflow.COMBINE_DRY_RUN_CHECKPOINT)
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        (self.root / "hostfile").write_text("192.0.2.1\n192.0.2.2\n")

    def test_modes_and_independent_quantization_without_endpoints(self):
        for operator in ("dispatch", "combine", "both"):
            for dtype in ("bf16", "fp16"):
                for quant in ("0", "4"):
                    for mode in ("functional", "performance", "breakdown"):
                        with self.subTest(operator=operator, dtype=dtype, quant=quant, mode=mode), mock.patch.dict(os.environ, {
                                "TILEXR_VALIDATION_OPERATOR_MODE": operator, "TILEXR_VALIDATION_DTYPE": dtype,
                                "TILEXR_VALIDATION_DISPATCH_QUANT_MODE": quant, "TILEXR_VALIDATION_RUN_MODE": mode}):
                            config = workflow.Config("case-dry-run")
                            self.assertEqual(config.chain["operator_mode"], operator)
                            self.assertEqual(config.chain["dispatch_quant_mode"], int(quant))
                            self.assertEqual(config.chain["quant_mode"], int(quant))
                            self.assertEqual(config.chain["combine_quant_mode"], 0)
                            self.assertNotIn("comm_id", config.chain)
                            self.assertEqual(len(config.hosts()), 2)

    def test_combine_standalone_does_not_require_dispatch_route_epochs(self):
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": "combine",
                "TILEXR_VALIDATION_BS": "1", "TILEXR_VALIDATION_TOPK": "5"}):
            self.assertEqual(len(workflow.Config("case-dry-run").hosts()), 2)
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": "both"}):
                with self.assertRaisesRegex(ValueError, "balance_shift"):
                    workflow.Config("case-dry-run").hosts()

    def test_alias_accepts_matching_values_and_rejects_conflicts_and_unsupported_modes(self):
        with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_QUANT_MODE": "4"}):
            self.assertEqual(workflow.Config("case-dry-run").chain["dispatch_quant_mode"], 4)
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_DISPATCH_QUANT_MODE": "4"}):
                self.assertEqual(workflow.Config("case-dry-run").chain["quant_mode"], 4)
        for environment in ({"QUANT_MODE": "4", "DISPATCH_QUANT_MODE": "0"},
                            {"COMBINE_QUANT_MODE": "4"}, {"DISPATCH_QUANT_MODE": "1"},
                            {"OPERATOR_MODE": "other"}, {"DTYPE": "fp32"}, {"LOOPS": "0"},
                            {"WARMUP": "-1"}, {"FP8_FORMAT": "bad"}, {"BLOCK_DIM": "32"}):
            with self.subTest(environment=environment), mock.patch.dict(os.environ, {
                    "TILEXR_VALIDATION_" + key: value for key, value in environment.items()}), self.assertRaises(ValueError):
                workflow.Config("case-dry-run")
        for operator in ("combine", "both"):
            with mock.patch.dict(os.environ, {"TILEXR_VALIDATION_OPERATOR_MODE": operator,
                    "TILEXR_VALIDATION_CHECKPOINT": workflow.DISPATCH_CHECKPOINT,
                    "TILEXR_VALIDATION_BUILD_MODE": "production"}), self.assertRaisesRegex(ValueError, "C4"):
                workflow.Config("run")

    def test_local_controller_verifies_every_rank_and_rejects_invalid_identity_or_side_effect(self):
        os.environ["TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM"] = "32"
        config = workflow.Config("case-dry-run")
        manifest = dict(build_id="build", source={"commit": "a" * 40}, checkpoint=config.checkpoint)
        mutation = {}
        calls = []

        def execute(argv, log, **kwargs):
            calls.append(argv)
            self.assertEqual(Path(argv[0]).name, workflow.COMBINE_DRY_RUN_PROGRAM)
            self.assertEqual(argv[1], "--dry-run")
            self.assertLessEqual(kwargs["timeout"], config.timeout)
            if config.chain["block_dim"] == "auto":
                self.assertNotIn("TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM", kwargs["env"])
            else:
                self.assertEqual(kwargs["env"]["TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM"], "64")
            mapping = dict(zip(("host", "host_index", "local_rank", "device", "rank", "rank_size"), argv[5:]))
            mapping = {key: value if key == "host" else int(value) for key, value in mapping.items()}
            record = dict(workflow.rank_record(mapping, argv[2], manifest), pid=100 + mapping["rank"],
                          **config.chain, device_allocated=False, communicator_created=False,
                          kernel_launched=False, network_used=False)
            record.update(mutation)
            return workflow.COMBINE_DRY_RUN_MARKER + json.dumps(record) + "\n"

        with mock.patch.object(workflow, "verify_build", return_value=manifest), \
                mock.patch.object(workflow, "execute", side_effect=execute), redirect_stdout(io.StringIO()):
            self.assertEqual(workflow.case_dry_run(config), 0)
            self.assertEqual(len(calls), 16)
            config.chain["block_dim"] = "64"
            self.assertEqual(workflow.case_dry_run(config), 0)
            for values in ({"rank": 0}, {"build_id": "old"}, {"operator_mode": "both"},
                           {"combine_quant_mode": False}, {"network_used": True}, {"device_allocated": 0}):
                mutation.clear()
                mutation.update(values)
                with self.subTest(values=values):
                    self.assertNotEqual(workflow.case_dry_run(config), 0)
        summaries = [workflow.read_json(path) for path in config.logs.glob("dry-run-*/summary.json")]
        self.assertEqual(sum(report["status"] == "PASS" for report in summaries), 2)


if __name__ == "__main__":
    unittest.main()
