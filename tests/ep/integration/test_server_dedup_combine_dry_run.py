"""C3: run the shared C++ consumer without ACL, communicator or device calls."""
import argparse
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CHECKPOINT = "ep-sdedup-combine-p1-dry-run"
MARKER = "EP_SERVER_DEDUP_COMBINE_DRYRUN_PASS "
CONSUMER = None


class CombineDryRunTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tilexr-combine-c3-")
        self.addCleanup(self.temp.cleanup)
        # Deliberately omit rendezvous and profiling configuration. Any attempt
        # to initialize ACL is logged and fails before a device can be touched.
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(
            ("TILEXR_", "TEST_"))}
        self.env.update(TEST_TRACE_DIR=self.temp.name, TEST_FAIL_STAGE="init")
        self.configure(CHECKPOINT=CHECKPOINT, BS=96, H=7168, TOPK=6,
                       MOE_EXPERT_NUM=96, DTYPE="bf16")

    def configure(self, **values):
        for key, value in values.items():
            self.env["TILEXR_VALIDATION_" + key] = str(value)

    def invoke(self, *, rank=0, ranks=16, identity=None, dry_run=True, error=None):
        host, local = divmod(rank, 8)
        args = ["run-c3", "build-c3", "unknown", f"host-{host}",
                str(host), str(local), str(local), str(rank), str(ranks)]
        if identity:
            for index, value in identity.items():
                args[index] = str(value)
        command = [str(CONSUMER)] + (["--dry-run"] if dry_run else []) + args
        result = subprocess.run(command, env=self.env, text=True,
                                capture_output=True, timeout=5)
        self.assertEqual(list(Path(self.temp.name).iterdir()), [],
                         "dry-run invoked an ACL/comm/runtime substitute")
        lines = [line[len(MARKER):] for line in result.stdout.splitlines()
                 if line.startswith(MARKER)]
        if error:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual(lines, [])
            self.assertIn(error, result.stderr)
            return None
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(lines), 1, result.stdout)
        record = json.loads(lines[0])
        self.assertEqual(record["checkpoint"], CHECKPOINT)
        self.assertEqual(record["status"], "PASS")
        self.assertEqual(record["rank"], rank)
        self.assertEqual(record["rank_size"], ranks)
        self.assertEqual(record["host_index"], host)
        self.assertEqual(record["local_rank"], local)
        self.assertEqual(record["device"], local)
        for key in ("device_allocated", "communicator_created", "kernel_launched", "network_used"):
            self.assertIs(record[key], False)
        return record

    def test_operator_dtype_quantization_matrix(self):
        for op, dtype, quant, fmt in itertools.product(
                ("dispatch", "combine", "both"), ("bf16", "fp16"), (0, 4), ("e4m3", "e5m2")):
            with self.subTest(op=op, dtype=dtype, quant=quant, fmt=fmt):
                self.configure(OPERATOR_MODE=op, DTYPE=dtype,
                               DISPATCH_QUANT_MODE=quant, COMBINE_QUANT_MODE=0, FP8_FORMAT=fmt)
                row = self.invoke()
                self.assertEqual(row["operator_mode"], op)
                self.assertEqual(row["dtype"], dtype)
                self.assertEqual(row["dispatch_quant_mode"], quant)
                self.assertEqual(row["quant_mode"], quant)
                self.assertEqual(row["combine_quant_mode"], 0)
                self.assertEqual(row["fp8_format"], fmt)
                self.assertEqual(row["fp8_format_effective"], fmt if op != "combine" and quant else "none")
                self.assertEqual(row["dispatch_active"], op != "combine")
                self.assertEqual(row["combine_active"], op != "dispatch")
                self.assertEqual(row["dispatch_quantization_active"], op != "combine" and quant == 4)
                expected = ("none" if op == "dispatch" else "standalone_expert_out" if op == "combine"
                            else "separate_expert_out" if quant else "dispatch_expand_x")
                self.assertEqual(row["combine_input_source"], expected)
                self.assertEqual(row["tables_source"], {
                    "dispatch": "none", "combine": "standalone", "both": "dispatch_outputs"}[op])

    def test_default_and_legacy_alias(self):
        row = self.invoke()
        self.assertEqual(row["operator_mode"], "dispatch")
        self.assertEqual(row["dispatch_quant_mode"], 0)
        self.configure(QUANT_MODE=4)
        self.assertEqual(self.invoke()["dispatch_quant_mode"], 4)
        self.configure(DISPATCH_QUANT_MODE=4)
        self.assertEqual(self.invoke()["dispatch_quant_mode"], 4)
        self.configure(DISPATCH_QUANT_MODE=0)
        self.invoke(error="conflicting Dispatch quantization aliases")

    def test_modes_and_all_rank_mappings(self):
        for op, mode in itertools.product(("dispatch", "combine", "both"),
                                         ("functional", "performance", "breakdown")):
            self.configure(OPERATOR_MODE=op, RUN_MODE=mode, LOOPS=3, WARMUP=2)
            row = self.invoke()
            self.assertEqual((row["run_mode"], row["loops"], row["warmup"]), (mode, 3, 2))
        for ranks in (16, 64, 128):
            self.configure(OPERATOR_MODE="both", MOE_EXPERT_NUM=ranks * 2, TOPK=2)
            for rank in range(ranks):
                self.invoke(rank=rank, ranks=ranks)

    def test_invalid_configuration(self):
        cases = [("OPERATOR_MODE", "other", "operator_mode"),
                 ("DTYPE", "fp32", "DTYPE"), ("DISPATCH_QUANT_MODE", 1, "quant_mode"),
                 ("COMBINE_QUANT_MODE", 4, "combine_quant_mode"),
                 ("FP8_FORMAT", "fp8", "fp8_format"), ("LOOPS", 0, "LOOPS"),
                 ("WARMUP", 4294967296, "WARMUP"), ("RUN_MODE", "other", "RUN_MODE"),
                 ("BS", 0, "layout"), ("H", 0, "layout"),
                 ("H", 4294967296, "layout"), ("TOPK", 64, "layout"),
                 ("MOE_EXPERT_NUM", 1536, "layout"), ("MOE_EXPERT_NUM", 17, "divisible"),
                 ("SEED", "01", "canonical"), ("ROUTE_MODE", "other", "ROUTE_MODE")]
        baseline = dict(self.env)
        for key, value, message in cases:
            with self.subTest(key=key, value=value):
                self.env = dict(baseline)
                self.configure(**{key: value})
                self.invoke(error=message)
        self.env = baseline
        self.env["TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM"] = "32"
        self.invoke(error="block_dim")
        self.env["TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM"] = "64"
        self.invoke()

    def test_invalid_rank_mapping(self):
        for identity in ({4: 1}, {5: 8}, {6: 1}, {7: 1}, {8: 8}, {8: 129}):
            with self.subTest(identity=identity):
                self.invoke(identity=identity, error="mapping" if 6 not in identity else "device == local_rank")

    def test_combine_has_no_dispatch_route_or_count_ub_constraint(self):
        self.configure(OPERATOR_MODE="combine", BS=1, TOPK=3, MOE_EXPERT_NUM=32)
        self.invoke()
        self.configure(OPERATOR_MODE="both")
        self.invoke(error="complete global token epochs")
        self.configure(OPERATOR_MODE="combine", BS=1048576, TOPK=32, MOE_EXPERT_NUM=1024)
        self.invoke()
        self.configure(OPERATOR_MODE="both")
        self.invoke(error="dispatch layout")

    def test_unimplemented_device_execution_is_rejected(self):
        self.invoke(dry_run=False, error="requires --dry-run")
        self.configure(CHECKPOINT="ep-sdedup-p2-dispatch")
        for op in ("combine", "both"):
            self.configure(OPERATOR_MODE=op)
            self.invoke(dry_run=False, error="require C4")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--consumer", type=Path, required=True)
    args = parser.parse_args()
    CONSUMER = args.consumer.resolve()
    if not CONSUMER.is_file() or not os.access(CONSUMER, os.X_OK):
        parser.error(f"missing executable consumer: {CONSUMER}")
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(CombineDryRunTests))
    if not result.wasSuccessful() or result.skipped:
        sys.exit(1)
    print(MARKER + "test=configuration")
