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
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools/validation"))
import measurements
from test_profiling_fixture import CAPTURE_ID


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
            "-I", str(ROOT / "src/comm"), "-I", str(ROOT / "src/ep/host"), "-I", str(ROOT / "src/ep/common"),
            str(SOURCE / "test_tilexr_ep_server_dedup_chain.cpp"),
            str(SOURCE / "server_dedup_chain_stub.cpp"),
            str(ROOT / "src/comm/tilexr_profiling_runtime.cpp"),
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

    def start(self, rank, *, environment=None, run="run-identity", stderr=subprocess.PIPE):
        env = dict(self.environment, TEST_RANK=str(rank))
        env.update(environment or {})
        if env.get("TILEXR_VALIDATION_RUN_MODE") == "breakdown":
            env.setdefault("TILEXR_PROFILING_CAPTURE_ID", CAPTURE_ID)
            env.setdefault("TILEXR_PROFILING_RECORDS_PATH", str(Path(self.trace.name) / f"rank-{rank}.records.v1"))
        process = subprocess.Popen([str(self.executable), run, "build-identity", "unknown", f"node-{rank // 8}",
            str(rank // 8), str(rank % 8), str(rank % 8), str(rank), "16"],
            env=env, stdout=subprocess.PIPE, stderr=stderr, text=True)
        self.processes.append(process)
        return process

    def trace_lines(self, rank):
        path = Path(self.trace.name) / str(rank)
        return path.read_text().splitlines() if path.exists() else []

    def records(self, output, prefix):
        return [json.loads(line[len(prefix) + 1:]) for line in output.splitlines() if line.startswith(prefix + " ")]

    def check_relay_links(self, records):
        first_hops, terminals = {}, []
        for record in records:
            first_count = terminal_count = 0
            for source, token, first_k, relay, t, k, count in record["relay_links"]:
                key = (source, token, first_k, relay)
                if k == first_k:
                    self.assertNotIn(key, first_hops)
                    self.assertEqual(relay, record["rank"])
                    self.assertGreater(count, 0)
                    first_hops[key] = (t, count)
                    first_count += 1
                else:
                    self.assertEqual(count, -1)
                    terminals.append((key, t, k))
                    terminal_count += 1
            self.assertEqual(first_count, record["first_hop_received"])
            self.assertEqual(terminal_count, record["relay_received"])
        members = {key: set() for key in first_hops}
        for key, t, k in terminals:
            self.assertIn(key, first_hops)
            self.assertEqual(t, first_hops[key][0])
            self.assertNotIn(k, members[key])
            members[key].add(k)
        for key, (_, count) in first_hops.items():
            self.assertEqual(len(members[key]), count)

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

    def test_single_dispatch_lifecycle_for_six_type_combinations(self):
        for dtype in ("bf16", "fp16"):
            for quant, format in (("0", "e4m3"), ("4", "e4m3"), ("4", "e5m2")):
                with self.subTest(dtype=dtype, quant=quant, format=format):
                    env = dict(TILEXR_VALIDATION_DTYPE=dtype, TILEXR_VALIDATION_QUANT_MODE=quant,
                               TILEXR_VALIDATION_FP8_FORMAT=format, TILEXR_VALIDATION_INPUT_DIR=self.trace.name)
                    processes = [self.start(rank, environment=env) for rank in range(16)]
                    sent, received, direct_sent, direct_received = 0, 0, 0, 0
                    records = []
                    for rank, process in enumerate(processes):
                        output, errors = process.communicate(timeout=15)
                        self.assertEqual(process.returncode, 0, output + errors)
                        self.assertNotIn("EP_SERVER_DEDUP_PROGRESS", errors)
                        record, = self.records(output, "EP_SERVER_DEDUP_DISPATCH_PASS")
                        records.append(record)
                        linked, = self.records(output, "EP_SERVER_DEDUP_CHAIN_PASS")
                        self.assertEqual(linked["peer_mem_bytes"], 4 * 1024 ** 3)
                        self.assertEqual(record["checkpoint"], "ep-sdedup-p2-dispatch")
                        self.assertEqual(record["dtype"], dtype)
                        self.assertEqual(record["quant_mode"], int(quant))
                        self.assertEqual(record["fp8_format"], format)
                        self.assertEqual(record["loops"], 1)
                        self.assertEqual(record["warmup"], 0)
                        self.assertEqual(record["cleanup"], "complete")
                        for key in ("first_hop_validated", "direct_validated", "relay_validated", "route_coverage_validated", "source_outputs_validated", "weights_unchanged", "dispatch_outputs_validated", "group_validated", "flags_cleared", "counter_cleared"):
                            self.assertIs(record[key], True)
                        sent += record["first_hop_packets"]; received += record["first_hop_received"]
                        direct_sent += record["direct_packets"]; direct_received += record["direct_received"]
                        self.assertEqual(record["direct_bytes"], record["direct_packets"] * record["token_stride"])
                        self.assertEqual(record["data_bytes"], record["first_hop_bytes"] + record["direct_bytes"] + record["relay_bytes"])
                        self.assertEqual(record["local_data_bytes"] + record["same_server_data_bytes"] + record["cross_server_data_bytes"],
                                         record["first_hop_bytes"] + record["direct_bytes"] + record["relay_bytes"])
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
                    self.assertEqual(sum(r["relay_packets"] for r in records), sum(r["relay_received"] for r in records))
                    self.assertEqual(sum(r["first_hop_packets"] + r["direct_packets"] + r["relay_packets"] for r in records), 16 * 2 * 3)
                    self.check_relay_links(records)
                    for path in Path(self.trace.name).iterdir(): path.unlink()

    def test_dispatch_tail_boundary_quantized_scales_and_changed_group_numbers(self):
        for quant, fault in (("0", None), ("4", None), ("4", "scale"), ("0", "duplicate-t")):
            with self.subTest(quant=quant, fault=fault):
                env = dict(TILEXR_VALIDATION_BS="3", TILEXR_VALIDATION_H="225", TILEXR_VALIDATION_TOPK="6",
                           TILEXR_VALIDATION_QUANT_MODE=quant, TILEXR_VALIDATION_LOOPS="3", TILEXR_VALIDATION_WARMUP="2")
                if fault:
                    env["TEST_PACKET_FAULT"] = fault
                processes = [self.start(rank, environment=env) for rank in range(16)]
                records, failed = [], 0
                for process in processes:
                    output, errors = process.communicate(timeout=20)
                    if process.returncode:
                        failed += 1
                        self.assertIn("stage=dispatch-read", errors)
                        self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
                    else:
                        record, = self.records(output, "EP_SERVER_DEDUP_DISPATCH_PASS")
                        records.append(record)
                if fault:
                    self.assertGreater(failed, 0)
                else:
                    self.assertEqual(failed, 0)
                    self.check_relay_links(records)
                for path in Path(self.trace.name).iterdir(): path.unlink()

    def test_checkpoint_and_loop_validation_precedes_initialization(self):
        cases = [{"TILEXR_VALIDATION_LOOPS": value} for value in ("", "0", "-1", "abc", "4294967296")]
        cases += [{"TILEXR_VALIDATION_WARMUP": value} for value in ("", "-1", "4294967296", "abc")]
        cases += [{"TILEXR_VALIDATION_RUN_MODE": value} for value in ("", "unknown")]
        cases += [{"TILEXR_VALIDATION_CHECKPOINT": value} for value in
                  ("ep-sdedup-p1-chain-single", "ep-sdedup-p1-chain-multi", "ep-sdedup-p1-profile", "ep-sdedup-p2-first-hop", "ep-sdedup-p2-source-send")]
        cases += [{"TILEXR_VALIDATION_QUANT_MODE": "3"}, {"TILEXR_VALIDATION_FP8_FORMAT": "invalid"}]
        for env in cases:
            with self.subTest(env=env):
                process = self.start(0, environment=env)
                output, errors = process.communicate(timeout=5)
                self.assertNotEqual(process.returncode, 0)
                self.assertNotIn("_PASS ", output)
                self.assertIn("stage=config", errors)
                self.assertEqual(self.trace_lines(0), [])

    def test_repeated_calls_reuse_buffers_and_sync_only_after_whole_batch(self):
        for mode in ("functional", "performance"):
            with self.subTest(mode=mode):
                env = dict(TILEXR_VALIDATION_RUN_MODE=mode, TILEXR_VALIDATION_LOOPS="3", TILEXR_VALIDATION_WARMUP="2")
                processes = [self.start(rank, environment=env) for rank in range(16)]
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=15)
                    self.assertEqual(process.returncode, 0, output + errors)
                    record, = self.records(output, "EP_SERVER_DEDUP_DISPATCH_PASS")
                    self.assertEqual((record["submitted"], record["completed"], record["warmup_submitted"],
                                      record["warmup_completed"]), (3, 3, 2, 2))
                    calls = self.trace_lines(rank)
                    self.assertEqual(calls.count("comm-init-sized"), 1)
                    self.assertEqual(calls.count("create-stream"), 1)
                    self.assertEqual(calls.count("launch"), 5)
                    self.assertEqual(calls.count("sync"), 1)
                    self.assertGreater(calls.index("sync"), max(i for i, call in enumerate(calls) if call == "launch"))
                    self.assertEqual(calls.count("malloc"), 10)
                    self.assertEqual(calls.count("free"), 10)
                for path in Path(self.trace.name).iterdir(): path.unlink()

    def test_repeated_launch_failure_preserves_partial_batch_accounting(self):
        for failed_call, batch, submission, submitted, warmed in ((2, "warmup", 2, 0, 1), (4, "measurement", 2, 1, 2)):
            with self.subTest(failed_call=failed_call):
                env = dict(TILEXR_VALIDATION_LOOPS="3", TILEXR_VALIDATION_WARMUP="2")
                processes = [self.start(rank, environment=dict(env, **(
                    {"TEST_FAIL_STAGE": "launch", "TEST_FAIL_CALL": str(failed_call)} if rank == 1 else {})))
                    for rank in range(16)]
                output, errors = processes[1].communicate(timeout=10)
                self.assertNotEqual(processes[1].returncode, 0)
                self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
                self.assertIn(f"batch={batch} submission={submission} submitted={submitted} completed=0", errors)
                self.assertIn(f"warmup_submitted={warmed} warmup_completed=0", errors)
                calls = self.trace_lines(1)
                self.assertEqual(calls.count("launch"), failed_call)
                self.assertNotIn("sync", calls)
                self.assertNotIn("free", calls)
                self.stop(); self.processes.clear()
                for path in Path(self.trace.name).iterdir(): path.unlink()

    def test_repeated_calls_reject_stale_device_completion(self):
        env = dict(TILEXR_VALIDATION_LOOPS="3", TILEXR_VALIDATION_WARMUP="2")
        processes = [self.start(rank, environment=dict(env, **(
            {"TEST_ROUND_FAULT": "stale"} if rank == 1 else {}))) for rank in range(16)]
        for rank, process in enumerate(processes):
            output, errors = process.communicate(timeout=15)
            if rank == 1:
                self.assertNotEqual(process.returncode, 0)
                self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
                self.assertIn("stage=round-read", errors)
                self.assertIn("expected=5 actual=4", errors)
            else:
                self.assertEqual(process.returncode, 0, errors)

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
        for fault in ("hidden", "flag-first", "flag-last", "mask", "source-weight", "input-weight", "missing-output",
                      "assist-source", "assist-token", "assist-k", "assist-weight", "assist-role", "assist-count",
                      "assist-relay", "assist-t", "group-row", "group-slot", "group-weight", "row-order", "counter"):
            with self.subTest(fault=fault):
                processes = [self.start(rank, environment={"TEST_PACKET_FAULT": fault}) for rank in range(16)]
                failures = 0
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=15)
                    if process.returncode:
                        failures += 1
                        self.assertIn("stage=dispatch-read", errors)
                        self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
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
                        self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
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

    def test_breakdown_uses_standard_export_after_communication_cleanup(self):
        config = dict(run_mode="breakdown", loops=3, warmup=2, block_dim="auto")
        for fault in ("", "missing", "stale", "capture", "overflow"):
            env = dict(TILEXR_VALIDATION_RUN_MODE="breakdown", TILEXR_VALIDATION_LOOPS="3",
                       TILEXR_VALIDATION_WARMUP="2", TEST_PROFILE_FAULT=fault)
            processes = [self.start(rank, environment=env) for rank in range(16)]
            for rank, process in enumerate(processes):
                output, errors = process.communicate(timeout=30)
                with self.subTest(rank=rank, fault=fault):
                    self.assertEqual(process.returncode, 0, output + errors)
                    self.assertEqual(len(self.records(output, "EP_SERVER_DEDUP_DISPATCH_PASS")), 1)
                    marker = self.records(output, "EP_SERVER_DEDUP_BREAKDOWN_PASS")[0]
                    self.assertEqual(marker["schema"], measurements.RECORDS_SCHEMA)
                    self.assertEqual(marker["launches"], 5)
                    calls = self.trace_lines(rank)
                    self.assertEqual(calls.count("sync"), 1)
                    self.assertEqual(calls.count("profiling-export"), 1)
                    self.assertGreater(calls.index("profiling-export"), calls.index("free"))
                    self.assertLess(calls.index("profiling-export"), calls.index("finalize"))
                    if rank == 0:
                        path = Path(marker["records_path"])
                        report = Path(self.trace.name) / f"report-{fault or 'valid'}"
                        if fault in ("missing", "capture"):
                            with self.assertRaises(ValueError):
                                measurements.breakdown_record(path, config, rank, CAPTURE_ID, report)
                        else:
                            record = measurements.breakdown_record(path, config, rank, CAPTURE_ID, report)
                            self.assertEqual(record["status"], "incomplete" if fault else "complete")
                            if not fault:
                                self.assertEqual(record["sample_us"], [0.7] * 3)
                                self.assertEqual(record["stages_us"]["atomic"]["sample_count"], 3 * 64)
            shutil.rmtree(self.trace.name)
            Path(self.trace.name).mkdir()

    def test_profile_capacity_overflow_is_rejected_before_communicator_creation(self):
        process = self.start(0, environment=dict(TILEXR_VALIDATION_RUN_MODE="breakdown",
            TILEXR_VALIDATION_LOOPS="1024", TILEXR_VALIDATION_WARMUP="0"))
        output, errors = process.communicate(timeout=5)
        self.assertNotEqual(process.returncode, 0, output + errors)
        self.assertIn("profiling capture capacity", errors)
        self.assertNotIn("comm-init", self.trace_lines(0))

    def test_breakdown_progress_is_flushed_before_blocking_api(self):
        for api, stage in (("launch", "launch"), ("sync", "stream-sync"),
                           ("comm-destroy", "cleanup-communicator")):
            with self.subTest(api=api):
                resume = Path(self.trace.name) / "resume"
                progress = Path(self.trace.name) / "progress.log"
                env = dict(TILEXR_VALIDATION_RUN_MODE="breakdown", TILEXR_VALIDATION_LOOPS="3",
                           TILEXR_VALIDATION_WARMUP="2")
                with progress.open("w") as log:
                    process = self.start(0, environment=dict(env, TEST_PAUSE_STAGE=api,
                        TEST_RESUME_FILE=str(resume)), stderr=log)
                    peers = [self.start(rank, environment=env) for rank in range(1, 16)]
                    deadline = time.monotonic() + 8
                    while api not in self.trace_lines(0) and time.monotonic() < deadline:
                        self.assertIsNone(process.poll(), progress.read_text())
                        time.sleep(0.02)
                    self.assertIn(api, self.trace_lines(0))
                    # The API is still blocked. Evidence must already be on
                    # disk, without an exception, return, or process shutdown.
                    self.assertIsNone(process.poll())
                    lines = progress.read_text().splitlines()
                    self.assertTrue(lines)
                    self.assertIn(f"stage={stage} ", lines[-1])
                    if api == "sync":
                        self.assertIn("submitted=3 completed=0 warmup_submitted=2 warmup_completed=0", lines[-1])
                    resume.touch()
                    for current in [process] + peers:
                        output, errors = current.communicate(timeout=15)
                        self.assertEqual(current.returncode, 0, output + (errors or ""))
                        self.assertEqual(len(self.records(output, "EP_SERVER_DEDUP_DISPATCH_PASS")), 1)
                entries = [dict(item.split("=", 1) for item in line.split()[1:])
                           for line in progress.read_text().splitlines()
                           if line.startswith("EP_SERVER_DEDUP_PROGRESS ")]
                self.assertEqual([entry["stage"] for entry in entries], [
                    "acl-init", "device", "communicator", "allocation", "chain-barrier", "launch", "launch",
                    "stream-sync", "batch-barrier", "round-read", "count-read", "dispatch-read",
                    "cleanup-buffers", "cleanup-communicator", "cleanup-stream", "cleanup-device", "cleanup-acl"])
                launches = [entry for entry in entries if entry["stage"] == "launch"]
                self.assertEqual([entry["batch"] for entry in launches], ["warmup", "measurement"])
                self.assertEqual([entry["submission"] for entry in launches], ["1", "1"])
                self.stop(); self.processes.clear()
                for path in Path(self.trace.name).iterdir(): path.unlink()

    def test_launch_and_async_errors_do_not_release_unconfirmed_round(self):
        for stage in ("launch", "sync"):
            with self.subTest(stage=stage):
                processes = [self.start(rank, environment={"TEST_FAIL_STAGE": stage} if rank == 1 else {})
                             for rank in range(16)]
                output, errors = processes[1].communicate(timeout=10)
                self.assertNotEqual(processes[1].returncode, 0)
                self.assertIn("EP_SERVER_DEDUP_CHAIN_PASS", output)
                self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
                self.assertIn(f"stage={stage if stage == 'launch' else 'stream-sync'}", errors)
                self.assertIn("completed=0", errors)
                self.assertIn("ret=91", errors)
                self.assertNotIn("free", self.trace_lines(1))
                self.assertNotIn("comm-destroy", self.trace_lines(1))
                self.stop(); self.processes.clear()
                for path in Path(self.trace.name).iterdir(): path.unlink()

    def test_cleanup_failure_does_not_print_dispatch_pass(self):
        processes = [self.start(rank, environment={"TEST_FAIL_STAGE": "comm-destroy"} if rank == 1 else {})
                     for rank in range(16)]
        for rank, process in enumerate(processes):
            output, errors = process.communicate(timeout=15)
            if rank == 1:
                self.assertNotEqual(process.returncode, 0)
                self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
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
        self.assertNotIn("EP_SERVER_DEDUP_DISPATCH_PASS", output)
        self.assertIn("stage=batch-barrier batch=measurement submission=1 submitted=1 completed=0", errors)
        self.assertIn("warmup_submitted=0 warmup_completed=0", errors)
        self.assertIn("mismatched run/build/rank/phase", errors)
        calls = self.trace_lines(0)
        self.assertEqual(calls.count("launch"), 1)
        self.assertEqual(calls.count("sync"), 1)
        self.assertNotIn("free", calls)


if __name__ == "__main__":
    unittest.main()
