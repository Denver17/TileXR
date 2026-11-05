"""Automatic execution policy is independent of the fixed wire layout."""
import json
import platform
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest import mock

import workflow


class DedupPolicyTests(unittest.TestCase):
    def test_installed_policy_resolves_each_shape_and_preserves_layout(self):
        for policy, layout in (("auto", True), ("0", False), ("1", True), (None, False), (None, True)):
            for bs in (1, 31, 32, 33, 256):
                with self.subTest(policy=policy, bs=bs), tempfile.TemporaryDirectory() as directory:
                    manifest = dict(server_dedup_enabled=layout, server_dedup_protocol=2,
                        server_dedup_core_config=dict(dispatch_send_core_num=32, combine_send_core_num=32, dispatch_count_core_num=32),
                        mode="host", checkpoint=None, server_dedup_contract=workflow.DISPATCH_ABI, server_dedup_combine_contract=workflow.combine_contract(), server_dedup_profiling=False,
                        architecture=platform.machine(), artifacts={})
                    if policy is not None:
                        manifest["server_dedup_policy"] = policy
                    install = Path(directory)
                    (install / "manifest.json").write_text(json.dumps(manifest))
                    config = SimpleNamespace(install=install, chain=dict(bs=bs, run_mode="functional"), mode="host",
                        checkpoint=None, action="run")
                    with mock.patch.object(workflow, "artifact_files", return_value={}):
                        workflow.verify_build(config)
                    expected = bs > 32 if policy == "auto" else layout
                    self.assertIs(config.chain["server_dedup_enabled"], expected)
                    self.assertIs(config.chain["server_dedup_layout_enabled"], layout)
                    self.assertEqual(config.chain["server_dedup_policy"], policy or str(int(layout)))

    def test_auto_direct_wire_stride_uses_reserved_read_index_space(self):
        # h=224 leaves exactly 480 payload bytes before the read-index reservation.
        shape = dict(bs=32, h=224, topk=6, quant_mode=0, server_dedup_enabled=False)
        self.assertEqual(workflow.source_send_stride(shape), 512)
        self.assertEqual(workflow.source_send_stride(dict(shape, server_dedup_layout_enabled=True)), 1024)
        self.assertEqual(workflow.source_send_stride(dict(shape, bs=33,
            server_dedup_enabled=True, server_dedup_layout_enabled=True)), 1024)

    def test_invalid_policy_layout_pairs_fail_before_launch(self):
        for policy, layout in (("auto", False), ("0", True), ("1", False), ("bogus", True), (2, True)):
            with self.subTest(policy=policy, layout=layout), self.assertRaises(ValueError):
                workflow.dedup_policy(dict(server_dedup_policy=policy, server_dedup_enabled=layout))


if __name__ == "__main__":
    unittest.main()
