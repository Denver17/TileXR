"""真实 C++ consumer 的 Host 行为测试；替身不证明 CANN 编译或 NPU 执行。"""
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[3]
SOURCE = Path(__file__).resolve().parent
ACL_HEADER = """
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "acl_base.h"
#define ACL_SUCCESS 0
typedef enum { ACL_MEM_MALLOC_HUGE_FIRST = 0 } aclrtMemMallocPolicy;
typedef enum { ACL_MEMCPY_HOST_TO_DEVICE = 1, ACL_MEMCPY_DEVICE_TO_HOST = 2 } aclrtMemcpyKind;
typedef enum { ACL_DEV_ATTR_VECTOR_CORE_NUM = 1 } aclrtDevAttr;
extern "C" {
aclError aclInit(const char *);
aclError aclFinalize();
aclError aclrtSetDevice(int32_t);
aclError aclrtResetDevice(int32_t);
aclError aclrtGetDevice(int32_t *);
aclError aclrtGetDeviceInfo(uint32_t, aclrtDevAttr, int64_t *);
const char *aclrtGetSocName();
aclError aclrtCreateStream(aclrtStream *);
aclError aclrtDestroyStream(aclrtStream);
aclError aclrtSynchronizeStream(aclrtStream);
aclError aclrtMalloc(void **, size_t, aclrtMemMallocPolicy);
aclError aclrtFree(void *);
aclError aclrtMemset(void *, size_t, int32_t, size_t);
aclError aclrtMemcpy(void *, size_t, const void *, size_t, aclrtMemcpyKind);
}
"""


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class ChainConsumerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++")
        if compiler is None or os.name != "posix":
            raise unittest.SkipTest("POSIX C++14 compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="tilexr-chain-consumer-")
        cls.folder = Path(cls.temp.name)
        include = cls.folder / "include" / "acl"
        include.mkdir(parents=True)
        (include / "acl_base.h").write_text("#pragma once\ntypedef int aclError;\ntypedef void *aclrtStream;\n")
        (include / "acl.h").write_text(ACL_HEADER)
        cls.executable = cls.folder / "consumer"
        result = subprocess.run([
            compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pthread", "-DTILEXR_EP_MEMORY_BOUNDARY_TEST=1",
            "-I", str(include.parent), "-I", str(ROOT / "src/include"),
            "-I", str(ROOT / "src/ep/host"), "-I", str(ROOT / "src/ep/common"),
            str(SOURCE / "test_tilexr_ep_server_dedup_chain.cpp"),
            str(SOURCE / "server_dedup_chain_stub.cpp"),
            str(ROOT / "src/ep/host/ep_memory_server_dedup_layout.cpp"),
            str(ROOT / "src/ep/host/ep_memory_layout.cpp"),
            str(ROOT / "src/ep/host/ep_layout.cpp"),
            "-o", str(cls.executable),
        ], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.trace = tempfile.TemporaryDirectory(dir=self.folder)
        self.addCleanup(self.trace.cleanup)
        self.processes = []
        self.addCleanup(self.stop)
        comm, barrier = free_port(), free_port()
        while barrier == comm:
            barrier = free_port()
        self.endpoint = ("127.0.0.1", barrier)
        self.environment = dict(os.environ, TILEXR_VALIDATION_BS="2", TILEXR_VALIDATION_H="16",
            TILEXR_VALIDATION_TOPK="3", TILEXR_VALIDATION_MOE_EXPERT_NUM="32", TILEXR_VALIDATION_DTYPE="bf16",
            TILEXR_COMM_ID=f"127.0.0.1:{comm}", TILEXR_DEMO_BARRIER_ADDR=f"127.0.0.1:{barrier}",
            TEST_TRACE_DIR=self.trace.name)
        self.environment.pop("TILEXR_VALIDATION_CHECKPOINT", None)
        self.environment.pop("TILEXR_VALIDATION_LOOPS", None)
        self.environment.pop("TILEXR_VALIDATION_WARMUP", None)
        self.environment.pop("TILEXR_VALIDATION_RUN_MODE", None)
        self.environment.pop("TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM", None)

    def stop(self):
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

    def start(self, rank, *, environment=None, run="run-identity"):
        env = dict(self.environment, TEST_RANK=str(rank))
        env.update(environment or {})
        process = subprocess.Popen([str(self.executable), run, "build-identity", "unknown", f"node-{rank // 8}",
            str(rank // 8), str(rank % 8), str(rank % 8), str(rank), "16"],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.processes.append(process)
        return process

    def trace_lines(self, rank):
        path = Path(self.trace.name) / str(rank)
        return path.read_text().splitlines() if path.exists() else []

    def records(self, output, prefix):
        return [json.loads(line[len(prefix) + 1:]) for line in output.splitlines() if line.startswith(prefix + " ")]

    def barrier_peer(self, rank, phase, run=b"run-identity"):
        deadline = time.monotonic() + 5
        while True:
            try:
                peer = socket.create_connection(self.endpoint, timeout=1)
                break
            except ConnectionRefusedError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.02)
        peer.sendall(struct.pack("!4I", 1, phase, rank, 16) +
                     run.ljust(128, b"\0") + b"build-identity".ljust(128, b"\0"))
        return peer

    def test_memory_boundary_all_ranks_validate_capacity_and_roundtrip(self):
        environment = {"TILEXR_VALIDATION_CHECKPOINT": "ep-sdedup-p1-memory-boundary"}
        processes = [self.start(rank, environment=environment) for rank in range(16)]
        for rank, process in enumerate(processes):
            output, errors = process.communicate(timeout=20)
            self.assertEqual(process.returncode, 0, output + errors)
            linked, = self.records(output, "EP_SERVER_DEDUP_CHAIN_PASS")
            record, = self.records(output, "EP_SERVER_DEDUP_MEMORY_BOUNDARY_PASS")
            self.assertEqual(linked["peer_mem_bytes"], 4 * 1024 ** 3)
            self.assertEqual(record["boundary_offset"], 4 * 1024 ** 3 - 32)
            self.assertEqual(record["rank"], rank)
            for key in ("initial_zero", "device_initial_zero", "device_readback_ff", "host_boundary_ff", "guard_zero"):
                self.assertIs(record[key], True)
            calls = self.trace_lines(rank)
            self.assertEqual(calls.count("comm-init-sized"), 1)
            self.assertEqual(calls.count("boundary-launch"), 1)
            self.assertNotIn("launch", calls)  # No Dispatch call or old checkpoint regression.
            self.assertEqual(calls[-1], "finalize")

    def test_memory_boundary_rejects_bad_capacity(self):
        process = self.start(0, environment={"TILEXR_VALIDATION_CHECKPOINT": "ep-sdedup-p1-memory-boundary",
                                           "TEST_BOUNDARY_FAULT": "capacity"})
        output, errors = process.communicate(timeout=5)
        self.assertNotEqual(process.returncode, 0)
        self.assertNotIn("_PASS", output)
        self.assertIn("not exactly 4 GiB", errors)

    def test_memory_boundary_detects_missing_operations_and_corruption(self):
        for fault in ("initial", "no-launch", "no-write", "no-read", "guard"):
            with self.subTest(fault=fault):
                process = self.start(0, environment={"TILEXR_VALIDATION_CHECKPOINT": "ep-sdedup-p1-memory-boundary",
                                                   "TEST_BOUNDARY_FAULT": fault})
                peers = []
                try:
                    peers = [self.barrier_peer(rank, 0) for rank in range(1, 16)]
                    output, errors = process.communicate(timeout=5)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertNotIn("EP_SERVER_DEDUP_MEMORY_BOUNDARY_PASS", output)
                    self.assertIn("ERROR", errors)
                    self.assertIn("expected=", errors)
                finally:
                    for peer in peers:
                        peer.close()

    def test_full_round_lifecycle_for_both_dtypes(self):
        cases = [("single", "functional", dtype, None, None, 1, 0) for dtype in ("bf16", "fp16")]
        for run_mode in ("functional", "performance"):
            default_loops, default_warmup = (1, 0) if run_mode == "functional" else (50, 50)
            cases += [("multi", run_mode, "bf16", None, None, default_loops, default_warmup),
                      ("multi", run_mode, "fp16", "5", "2", 5, 2)]
        for mode, run_mode, dtype, loops, warmup, count, warmup_count in cases:
            with self.subTest(mode=mode, run_mode=run_mode, dtype=dtype, loops=loops):
                environment = {"TILEXR_VALIDATION_DTYPE": dtype}
                if mode == "multi":
                    environment["TILEXR_VALIDATION_CHECKPOINT"] = "ep-sdedup-p1-chain-multi"
                    environment["TILEXR_VALIDATION_RUN_MODE"] = run_mode
                if loops is not None:
                    environment["TILEXR_VALIDATION_LOOPS"] = loops
                if warmup is not None:
                    environment["TILEXR_VALIDATION_WARMUP"] = warmup
                processes = [self.start(rank, environment=environment) for rank in range(16)]
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=15)
                    self.assertEqual(process.returncode, 0, errors)
                    chain = self.records(output, "EP_SERVER_DEDUP_CHAIN_PASS")
                    marker = "EP_SERVER_DEDUP_" + mode.upper() + "_ROUND_PASS"
                    final = self.records(output, marker)
                    self.assertEqual(len(chain), 1)
                    self.assertEqual(len(final), 1)
                    self.assertEqual(chain[0]["peer_count"], 16)
                    self.assertEqual(chain[0]["checkpoint"], f"ep-sdedup-p1-chain-{mode}")
                    self.assertEqual(final[0]["checkpoint"], chain[0]["checkpoint"])
                    self.assertIsNone(final[0]["commit"])
                    self.assertEqual(final[0]["rank"], rank)
                    self.assertEqual(final[0]["dtype"], dtype)
                    self.assertEqual(final[0]["cleanup"], "complete")
                    self.assertEqual(final[0]["submitted"], count)
                    self.assertEqual(final[0]["completed"], count)
                    self.assertEqual(final[0]["warmup_submitted"], warmup_count)
                    self.assertEqual(final[0]["warmup_completed"], warmup_count)
                    for record in (chain[0], final[0]):
                        self.assertEqual(record["loops"], count)
                        self.assertEqual(record["warmup"], warmup_count)
                        self.assertEqual(record["run_mode"], run_mode)
                        self.assertEqual(record["route_mode"], "balance_shift")
                        self.assertEqual(record["seed"], 2026)
                    self.assertNotIn("timing_us", final[0])
                    calls = self.trace_lines(rank)
                    self.assertEqual(calls.count("malloc"), 9)
                    self.assertEqual(calls.count("free"), 9)
                    self.assertEqual(calls.count("comm-init"), 1)
                    self.assertEqual(calls.count("comm-init-sized"), 1)
                    self.assertEqual(self.records(output, "EP_SERVER_DEDUP_CHAIN_PASS")[0]["peer_mem_bytes"],
                                     4 * 1024 ** 3)
                    self.assertEqual(calls.count("create-stream"), 1)
                    expected = (["launch"] * warmup_count + ["sync"]) if warmup_count else []
                    expected += ["launch"] * count + ["sync"]
                    expected += ["memcpy", "memcpy"]
                    self.assertEqual(calls[calls.index("launch"):calls.index("free")], expected)
                    self.assertLess(calls.index("launch"), calls.index("sync"))
                    self.assertLess(calls.index("sync"), calls.index("free"))
                    self.assertLess(calls.index("comm-destroy"), calls.index("reset-device"))
                    self.assertEqual(calls[-1], "finalize")
                for rank in range(16):
                    (Path(self.trace.name) / str(rank)).unlink()

    def test_checkpoint_and_loop_validation_precedes_initialization(self):
        cases = [{"TILEXR_VALIDATION_LOOPS": loops} for loops in
                 ("", "0", "-1", "abc", "4294967296", "9223372036854775808")]
        cases += [{"TILEXR_VALIDATION_WARMUP": warmup} for warmup in ("", "-1", "4294967296", "abc")]
        cases += [{"TILEXR_VALIDATION_RUN_MODE": mode} for mode in ("", "unknown", "breakdown")]
        cases += [{"TILEXR_VALIDATION_CHECKPOINT": "ep-sdedup-p1-chain-single", key: value}
                  for key, value in (("TILEXR_VALIDATION_LOOPS", "2"), ("TILEXR_VALIDATION_WARMUP", "1"),
                                     ("TILEXR_VALIDATION_RUN_MODE", "performance"))]
        for values in cases:
            with self.subTest(values=values):
                environment = dict(TILEXR_VALIDATION_CHECKPOINT="ep-sdedup-p1-chain-multi")
                environment.update(values)
                process = self.start(0, environment=environment)
                output, errors = process.communicate(timeout=5)
                self.assertNotEqual(process.returncode, 0)
                self.assertNotIn("_PASS ", output)
                self.assertIn("stage=config", errors)
                self.assertEqual(self.trace_lines(0), [])
        process = self.start(0, environment={"TILEXR_VALIDATION_CHECKPOINT": "ep-sdedup-p1-profile"})
        output, errors = process.communicate(timeout=5)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("unsupported TILEXR_VALIDATION_CHECKPOINT", errors)
        self.assertEqual(self.trace_lines(0), [])
        # 两个 uint32 上限之和仍在 generation 的 int64 范围内；初始化错误避免真的提交几十亿次。
        process = self.start(0, environment={"TILEXR_VALIDATION_CHECKPOINT": "ep-sdedup-p1-chain-multi",
            "TILEXR_VALIDATION_LOOPS": "4294967295", "TILEXR_VALIDATION_WARMUP": "4294967295",
            "TEST_FAIL_STAGE": "init"})
        output, errors = process.communicate(timeout=5)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("stage=acl-init", errors)
        self.assertEqual(self.trace_lines(0), ["init"])

    def test_prelaunch_errors_never_launch_or_pass(self):
        cases = [{"TEST_FAIL_STAGE": stage} for stage in (
            "init", "set-device", "create-stream", "comm-init", "comm-init-sized", "host-args", "device-args", "memcpy", "malloc")]
        cases += [{"TEST_SOC": "Ascend910B"}, {"TEST_MISSING_PEER": "1"},
                  {"TEST_SMALL_PEER": "1"}, {"TEST_LARGE_PEER": "1"}, {"TEST_DEVICE_MAPPING_MISMATCH": "1"},
                  {"TILEXR_VALIDATION_TOPK": "0"}, {"TILEXR_VALIDATION_H": "9223372036854775807"}]
        for environment in cases:
            with self.subTest(environment=environment):
                process = self.start(0, environment=environment)
                output, errors = process.communicate(timeout=5)
                self.assertNotEqual(process.returncode, 0)
                self.assertNotIn("_PASS ", output)
                self.assertIn("EP_SERVER_DEDUP_FAIL", errors)
                self.assertNotIn("launch", self.trace_lines(0))

    def test_count_corruption_never_reports_success(self):
        for fault in ("prefix", "expert", "unwritten"):
            with self.subTest(fault=fault):
                processes = [self.start(rank, environment={"TEST_COUNT_FAULT": fault} if rank == 1 else {})
                             for rank in range(16)]
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=15)
                    if rank == 1:
                        self.assertNotEqual(process.returncode, 0)
                        self.assertNotIn("EP_SERVER_DEDUP_SINGLE_ROUND_PASS", output)
                        self.assertIn("stage=count-read", errors)
                        line = next(line for line in errors.splitlines() if "stage=count-read" in line)
                        fields = dict(word.split('=', 1) for word in line.split() if '=' in word)
                        self.assertEqual(int(fields['global_expert']), 2)
                        self.assertEqual(int(fields['actual_window_start']), 0)
                        window = json.loads(fields['actual_window'])
                        self.assertEqual(window[0], int(fields['actual']))
                        if fault == 'unwritten':
                            self.assertEqual(window, [-1, -1, -1])
                        else:
                            self.assertEqual(int(fields['actual']), int(fields['expected']) + 1)
                        if fault != 'expert':
                            self.assertIn('sendCountsOut mismatch at 0', line)
                            self.assertEqual(int(fields['source_rank']), 0)
                        else:
                            self.assertIn('expertTokenNumsOut mismatch at 0', line)
                            self.assertNotIn('source_rank', fields)
                    else:
                        self.assertEqual(process.returncode, 0, errors)
                for rank in range(16):
                    (Path(self.trace.name) / str(rank)).unlink()

    def test_launch_and_async_errors_do_not_release_unconfirmed_round(self):
        cases = [
            # warmup, failing stage/call, batch, submission, submitted, warmup submitted/completed, calls
            (0, "launch", 3, "measurement", 3, 2, 0, 0, 3, 0),
            (0, "sync", 1, "measurement", 5, 5, 0, 0, 5, 1),
            (3, "launch", 3, "warmup", 3, 0, 2, 0, 3, 0),
            (3, "sync", 1, "warmup", 3, 0, 3, 0, 3, 1),
            (3, "launch", 6, "measurement", 3, 2, 3, 3, 6, 1),
            (3, "sync", 2, "measurement", 5, 5, 3, 3, 8, 2),
        ]
        for warmup, stage, nth, batch, index, submitted, warmup_submitted, warmup_completed, launches, syncs in cases:
            with self.subTest(warmup=warmup, stage=stage, nth=nth):
                environment = {"TILEXR_VALIDATION_CHECKPOINT": "ep-sdedup-p1-chain-multi",
                    "TILEXR_VALIDATION_LOOPS": "5", "TILEXR_VALIDATION_WARMUP": str(warmup)}
                failure = dict(environment, TEST_FAIL_STAGE=stage, TEST_FAIL_CALL=str(nth))
                processes = [self.start(rank, environment=failure if rank == 1 else environment)
                             for rank in range(16)]
                output, errors = processes[1].communicate(timeout=10)
                self.assertNotEqual(processes[1].returncode, 0)
                self.assertIn("EP_SERVER_DEDUP_CHAIN_PASS", output)
                self.assertNotIn("EP_SERVER_DEDUP_SINGLE_ROUND_PASS", output)
                self.assertNotIn("EP_SERVER_DEDUP_MULTI_ROUND_PASS", output)
                self.assertIn("ret=91", errors)
                self.assertIn(f"batch={batch} submission={index} submitted={submitted} completed=0", errors)
                self.assertIn(f"warmup_submitted={warmup_submitted} warmup_completed={warmup_completed}", errors)
                calls = self.trace_lines(1)
                self.assertEqual(calls.count("launch"), launches)
                self.assertEqual(calls.count("sync"), syncs)
                self.assertNotIn("free", calls)
                self.assertNotIn("comm-destroy", calls)
                self.assertNotIn("reset-device", calls)
                self.stop()
                for rank, process in enumerate(processes):
                    output, _ = process.communicate(timeout=5)
                    self.assertNotIn("EP_SERVER_DEDUP_SINGLE_ROUND_PASS", output)
                    self.assertNotIn("EP_SERVER_DEDUP_MULTI_ROUND_PASS", output)
                self.processes.clear()
                for path in Path(self.trace.name).iterdir():
                    path.unlink()

    def test_cleanup_failure_does_not_print_single_round_pass(self):
        for mode in ("single", "multi"):
            with self.subTest(mode=mode):
                environment = {"TILEXR_VALIDATION_CHECKPOINT": f"ep-sdedup-p1-chain-{mode}"}
                count = 1 if mode == "single" else 5
                environment["TILEXR_VALIDATION_LOOPS"] = str(count)
                processes = [self.start(rank, environment=dict(environment, TEST_FAIL_STAGE="comm-destroy")
                             if rank == 1 else environment) for rank in range(16)]
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=15)
                    if rank == 1:
                        self.assertNotEqual(process.returncode, 0)
                        self.assertNotIn("EP_SERVER_DEDUP_SINGLE_ROUND_PASS", output)
                        self.assertNotIn("EP_SERVER_DEDUP_MULTI_ROUND_PASS", output)
                        self.assertIn("stage=cleanup", errors)
                        self.assertIn(f"submission={count} submitted={count} completed={count}", errors)
                        self.assertEqual(self.trace_lines(rank)[-1], "finalize")
                    else:
                        self.assertEqual(process.returncode, 0, errors)
                for rank in range(16):
                    (Path(self.trace.name) / str(rank)).unlink()

    def test_barrier_rejects_wrong_run_phase_and_duplicate_rank(self):
        for phase, run, duplicate in ((1, b"run-identity", False), (0, b"other-run", False), (0, b"run-identity", True)):
            with self.subTest(phase=phase, run=run, duplicate=duplicate):
                process = self.start(0)
                peers = []
                try:
                    for _ in range(2 if duplicate else 1):
                        peers.append(self.barrier_peer(1, phase, run))
                    output, errors = process.communicate(timeout=5)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertNotIn("_PASS ", output)
                    self.assertIn("mismatched run/build/rank/phase", errors)
                finally:
                    for peer in peers:
                        peer.close()

    def test_multi_barrier_rejects_warmup_ack_as_measurement_completion(self):
        process = self.start(0, environment={"TILEXR_VALIDATION_CHECKPOINT": "ep-sdedup-p1-chain-multi",
            "TILEXR_VALIDATION_WARMUP": "2", "TILEXR_VALIDATION_LOOPS": "5"})
        # 模拟其余 15 ranks 完成建链和预热批，再在测量批结束时提交旧预热报文。
        for phase in range(2):
            peers = []
            try:
                for rank in range(1, 16):
                    peers.append(self.barrier_peer(rank, phase))
                for rank, peer in enumerate(peers, 1):
                    reply = b""
                    while len(reply) < 272:
                        chunk = peer.recv(272 - len(reply))
                        self.assertTrue(chunk)
                        reply += chunk
                    self.assertEqual(struct.unpack("!4I", reply[:16]), (1, phase, rank, 16))
            finally:
                for peer in peers:
                    peer.close()
        with self.barrier_peer(1, 1):
            output, errors = process.communicate(timeout=5)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("EP_SERVER_DEDUP_CHAIN_PASS", output)
        self.assertNotIn("EP_SERVER_DEDUP_MULTI_ROUND_PASS", output)
        self.assertIn("stage=batch-barrier batch=measurement submission=5 submitted=5 completed=0", errors)
        self.assertIn("warmup_submitted=2 warmup_completed=2", errors)
        self.assertIn("mismatched run/build/rank/phase", errors)
        calls = self.trace_lines(0)
        self.assertEqual(calls.count("launch"), 7)
        self.assertEqual(calls.count("sync"), 2)
        self.assertNotIn("free", calls)


if __name__ == "__main__":
    unittest.main()
