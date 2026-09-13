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

    def test_single_source_send_lifecycle_for_six_type_combinations(self):
        for dtype in ("bf16", "fp16"):
            for quant, format in (("0", "e4m3"), ("4", "e4m3"), ("4", "e5m2")):
                with self.subTest(dtype=dtype, quant=quant, format=format):
                    env = dict(TILEXR_VALIDATION_DTYPE=dtype, TILEXR_VALIDATION_QUANT_MODE=quant,
                               TILEXR_VALIDATION_FP8_FORMAT=format, TILEXR_VALIDATION_INPUT_DIR=self.trace.name)
                    processes = [self.start(rank, environment=env) for rank in range(16)]
                    sent, received, direct_sent, direct_received = 0, 0, 0, 0
                    for rank, process in enumerate(processes):
                        output, errors = process.communicate(timeout=15)
                        self.assertEqual(process.returncode, 0, output + errors)
                        record, = self.records(output, "EP_SERVER_DEDUP_SOURCE_SEND_PASS")
                        linked, = self.records(output, "EP_SERVER_DEDUP_CHAIN_PASS")
                        self.assertEqual(linked["peer_mem_bytes"], 4 * 1024 ** 3)
                        self.assertEqual(record["checkpoint"], "ep-sdedup-p2-source-send")
                        self.assertEqual(record["dtype"], dtype)
                        self.assertEqual(record["quant_mode"], int(quant))
                        self.assertEqual(record["fp8_format"], format)
                        self.assertEqual(record["loops"], 1)
                        self.assertEqual(record["warmup"], 0)
                        self.assertEqual(record["cleanup"], "complete")
                        for key in ("first_hop_validated", "direct_validated", "route_coverage_validated", "source_outputs_validated", "weights_unchanged"):
                            self.assertIs(record[key], True)
                        sent += record["first_hop_packets"]; received += record["first_hop_received"]
                        direct_sent += record["direct_packets"]; direct_received += record["direct_received"]
                        self.assertEqual(record["direct_bytes"], record["direct_packets"] * record["token_stride"])
                        self.assertEqual(record["local_data_bytes"] + record["same_server_data_bytes"] + record["cross_server_data_bytes"],
                                         record["first_hop_bytes"] + record["direct_bytes"])
                        calls = self.trace_lines(rank)
                        self.assertEqual(calls.count("malloc"), 10 + (quant == "4"))
                        self.assertEqual(calls.count("free"), calls.count("malloc"))
                        self.assertEqual(calls.count("launch"), 1)
                        self.assertEqual(calls.count("sync"), 1)
                        self.assertEqual(calls[-1], "finalize")
                        self.assertEqual((Path(self.trace.name) / f"rank-{rank}-x.bin").stat().st_size, 64)
                    self.assertGreater(sent, 0)
                    self.assertEqual(sent, received)
                    self.assertGreater(direct_sent, 0)
                    self.assertEqual(direct_sent, direct_received)
                    for path in Path(self.trace.name).iterdir(): path.unlink()

    def test_checkpoint_and_loop_validation_precedes_initialization(self):
        cases = [{"TILEXR_VALIDATION_LOOPS": value} for value in ("", "0", "2", "-1", "abc", "4294967295", "4294967296")]
        cases += [{"TILEXR_VALIDATION_WARMUP": value} for value in ("", "1", "-1", "4294967296", "abc")]
        cases += [{"TILEXR_VALIDATION_RUN_MODE": value} for value in ("", "unknown")]
        cases += [{"TILEXR_VALIDATION_CHECKPOINT": value} for value in
                  ("ep-sdedup-p1-chain-single", "ep-sdedup-p1-chain-multi", "ep-sdedup-p1-profile", "ep-sdedup-p2-first-hop")]
        cases += [{"TILEXR_VALIDATION_QUANT_MODE": "3"}, {"TILEXR_VALIDATION_FP8_FORMAT": "invalid"}]
        for env in cases:
            with self.subTest(env=env):
                process = self.start(0, environment=env)
                output, errors = process.communicate(timeout=5)
                self.assertNotEqual(process.returncode, 0)
                self.assertNotIn("_PASS ", output)
                self.assertIn("stage=config", errors)
                self.assertEqual(self.trace_lines(0), [])

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

    def test_packet_corruption_never_reports_success(self):
        for fault in ("hidden", "flag", "tail-q", "header-weight", "mask", "source-weight", "input-weight",
                      "direct-hidden", "direct-tail", "direct-missing", "direct-q", "direct-weight"):
            with self.subTest(fault=fault):
                processes = [self.start(rank, environment={"TEST_PACKET_FAULT": fault}) for rank in range(16)]
                failures = 0
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=15)
                    if process.returncode:
                        failures += 1
                        self.assertIn("stage=source-send-read", errors)
                        self.assertNotIn("EP_SERVER_DEDUP_SOURCE_SEND_PASS", output)
                    self.assertEqual(self.trace_lines(rank)[-1], "finalize")
                self.assertGreater(failures, 0, fault)
                for path in Path(self.trace.name).iterdir(): path.unlink()

    def test_count_corruption_never_reports_success(self):
        for fault in ("prefix", "expert", "unwritten"):
            with self.subTest(fault=fault):
                processes = [self.start(rank, environment={"TEST_COUNT_FAULT": fault} if rank == 1 else {})
                             for rank in range(16)]
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=15)
                    if rank == 1:
                        self.assertNotEqual(process.returncode, 0)
                        self.assertNotIn("EP_SERVER_DEDUP_SOURCE_SEND_PASS", output)
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

    def test_count_breakdown_reads_final_generation_and_rejects_bad_records(self):
        for fault in (None, "missing", "stale", "legacy", "core", "time", "issue-header", "issue-count", "issue-time"):
            with self.subTest(fault=fault):
                env = dict(TILEXR_VALIDATION_CHECKPOINT="ep-sdedup-p2-source-send",
                           TILEXR_VALIDATION_RUN_MODE="breakdown", TILEXR_VALIDATION_LOOPS="1",
                           TILEXR_VALIDATION_WARMUP="0")
                processes = [self.start(rank, environment=dict(env, **(
                    {"TEST_PROFILE_FAULT": fault} if rank == 1 and fault else {}))) for rank in range(16)]
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=15)
                    if rank == 1 and fault:
                        self.assertNotEqual(process.returncode, 0)
                        self.assertIn("stage=breakdown-read", errors)
                        self.assertNotIn("EP_SERVER_DEDUP_BREAKDOWN_PASS", output)
                        self.assertNotIn("EP_SERVER_DEDUP_SOURCE_SEND_PASS", output)
                    else:
                        self.assertEqual(process.returncode, 0, errors)
                        evidence = json.loads(next(line.split(" ", 1)[1] for line in output.splitlines()
                                                   if line.startswith("EP_SERVER_DEDUP_BREAKDOWN_PASS ")))
                        self.assertEqual(evidence["generation"], 1)
                        self.assertEqual(evidence["schema_version"], 4)
                        self.assertEqual(len(evidence["records"]), 64)
                        for core in evidence["records"][:48]:
                            self.assertEqual(bool(core["send_packets"]), core["core"] < evidence["bs"])
                            self.assertLessEqual(core["first_hop_count"], len(core["send_packets"]))
                        self.assertEqual(self.trace_lines(rank).count("sync"), 1)
                    (Path(self.trace.name) / str(rank)).unlink()

    def test_launch_and_async_errors_do_not_release_unconfirmed_round(self):
        for stage in ("launch", "sync"):
            with self.subTest(stage=stage):
                processes = [self.start(rank, environment={"TEST_FAIL_STAGE": stage} if rank == 1 else {})
                             for rank in range(16)]
                output, errors = processes[1].communicate(timeout=10)
                self.assertNotEqual(processes[1].returncode, 0)
                self.assertIn("EP_SERVER_DEDUP_CHAIN_PASS", output)
                self.assertNotIn("EP_SERVER_DEDUP_SOURCE_SEND_PASS", output)
                self.assertIn(f"stage={stage if stage == 'launch' else 'stream-sync'}", errors)
                self.assertIn("completed=0", errors)
                self.assertIn("ret=91", errors)
                self.assertNotIn("free", self.trace_lines(1))
                self.assertNotIn("comm-destroy", self.trace_lines(1))
                self.stop(); self.processes.clear()
                for path in Path(self.trace.name).iterdir(): path.unlink()

    def test_cleanup_failure_does_not_print_source_send_pass(self):
        processes = [self.start(rank, environment={"TEST_FAIL_STAGE": "comm-destroy"} if rank == 1 else {})
                     for rank in range(16)]
        for rank, process in enumerate(processes):
            output, errors = process.communicate(timeout=15)
            if rank == 1:
                self.assertNotEqual(process.returncode, 0)
                self.assertNotIn("EP_SERVER_DEDUP_SOURCE_SEND_PASS", output)
                self.assertIn("stage=cleanup", errors)
                self.assertIn("submission=1 submitted=1 completed=1", errors)
            else: self.assertEqual(process.returncode, 0, errors)
            self.assertEqual(self.trace_lines(rank)[-1], "finalize")

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

    def test_barrier_rejects_wrong_phase_as_measurement_completion(self):
        process = self.start(0)
        # 模拟其余 15 ranks 完成建链，再在测量完成后提交错误 phase。
        for phase in range(1):
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
        self.assertNotIn("EP_SERVER_DEDUP_SOURCE_SEND_PASS", output)
        self.assertIn("stage=batch-barrier batch=measurement submission=1 submitted=1 completed=0", errors)
        self.assertIn("warmup_submitted=0 warmup_completed=0", errors)
        self.assertIn("mismatched run/build/rank/phase", errors)
        calls = self.trace_lines(0)
        self.assertEqual(calls.count("launch"), 1)
        self.assertEqual(calls.count("sync"), 1)
        self.assertNotIn("free", calls)


if __name__ == "__main__":
    unittest.main()
