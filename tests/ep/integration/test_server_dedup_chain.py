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
typedef enum { ACL_MEMCPY_DEVICE_TO_HOST = 2 } aclrtMemcpyKind;
extern "C" {
aclError aclInit(const char *);
aclError aclFinalize();
aclError aclrtSetDevice(int32_t);
aclError aclrtResetDevice(int32_t);
aclError aclrtGetDevice(int32_t *);
const char *aclrtGetSocName();
aclError aclrtCreateStream(aclrtStream *);
aclError aclrtDestroyStream(aclrtStream);
aclError aclrtSynchronizeStream(aclrtStream);
aclError aclrtMalloc(void **, size_t, aclrtMemMallocPolicy);
aclError aclrtFree(void *);
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
            compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(include.parent), "-I", str(ROOT / "src/include"),
            "-I", str(ROOT / "src/ep/host"), "-I", str(ROOT / "src/ep/common"),
            str(SOURCE / "test_tilexr_ep_server_dedup_chain.cpp"),
            str(SOURCE / "server_dedup_chain_stub.cpp"),
            str(ROOT / "src/ep/host/ep_memory_server_dedup_layout.cpp"),
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

    def test_full_round_lifecycle_for_both_dtypes(self):
        for dtype in ("bf16", "fp16"):
            with self.subTest(dtype=dtype):
                processes = [self.start(rank, environment={"TILEXR_VALIDATION_DTYPE": dtype}) for rank in range(16)]
                for rank, process in enumerate(processes):
                    output, errors = process.communicate(timeout=15)
                    self.assertEqual(process.returncode, 0, errors)
                    chain = self.records(output, "EP_SERVER_DEDUP_CHAIN_PASS")
                    single = self.records(output, "EP_SERVER_DEDUP_SINGLE_ROUND_PASS")
                    self.assertEqual(len(chain), 1)
                    self.assertEqual(len(single), 1)
                    self.assertEqual(chain[0]["peer_count"], 16)
                    self.assertIsNone(single[0]["commit"])
                    self.assertEqual(single[0]["rank"], rank)
                    self.assertEqual(single[0]["dtype"], dtype)
                    self.assertEqual(single[0]["cleanup"], "complete")
                    calls = self.trace_lines(rank)
                    self.assertLess(calls.index("launch"), calls.index("sync"))
                    self.assertLess(calls.index("sync"), calls.index("free"))
                    self.assertLess(calls.index("comm-destroy"), calls.index("reset-device"))
                    self.assertEqual(calls[-1], "finalize")
                for rank in range(16):
                    (Path(self.trace.name) / str(rank)).unlink()

    def test_prelaunch_errors_never_launch_or_pass(self):
        cases = [{"TEST_FAIL_STAGE": stage} for stage in (
            "init", "set-device", "create-stream", "comm-init", "host-args", "device-args", "memcpy", "malloc")]
        cases += [{"TEST_SOC": "Ascend910B"}, {"TEST_MISSING_PEER": "1"},
                  {"TEST_SMALL_PEER": "1"}, {"TEST_DEVICE_MAPPING_MISMATCH": "1"},
                  {"TILEXR_VALIDATION_TOPK": "0"}, {"TILEXR_VALIDATION_H": "9223372036854775807"}]
        for environment in cases:
            with self.subTest(environment=environment):
                process = self.start(0, environment=environment)
                output, errors = process.communicate(timeout=5)
                self.assertNotEqual(process.returncode, 0)
                self.assertNotIn("_PASS ", output)
                self.assertIn("EP_SERVER_DEDUP_FAIL", errors)
                self.assertNotIn("launch", self.trace_lines(0))

    def test_launch_and_async_errors_do_not_release_unconfirmed_round(self):
        for stage in ("launch", "sync"):
            with self.subTest(stage=stage):
                processes = [self.start(rank, environment={"TEST_FAIL_STAGE": stage} if rank == 1 else {})
                             for rank in range(16)]
                output, errors = processes[1].communicate(timeout=10)
                self.assertNotEqual(processes[1].returncode, 0)
                self.assertIn("EP_SERVER_DEDUP_CHAIN_PASS", output)
                self.assertNotIn("EP_SERVER_DEDUP_SINGLE_ROUND_PASS", output)
                self.assertIn("ret=91", errors)
                calls = self.trace_lines(1)
                self.assertNotIn("free", calls)
                self.assertNotIn("comm-destroy", calls)
                self.assertNotIn("reset-device", calls)
                self.stop()
                self.processes.clear()
                for path in Path(self.trace.name).iterdir():
                    path.unlink()

    def test_cleanup_failure_does_not_print_single_round_pass(self):
        processes = [self.start(rank, environment={"TEST_FAIL_STAGE": "comm-destroy"} if rank == 1 else {})
                     for rank in range(16)]
        for rank, process in enumerate(processes):
            output, errors = process.communicate(timeout=15)
            if rank == 1:
                self.assertNotEqual(process.returncode, 0)
                self.assertNotIn("EP_SERVER_DEDUP_SINGLE_ROUND_PASS", output)
                self.assertIn("stage=cleanup", errors)
                self.assertEqual(self.trace_lines(rank)[-1], "finalize")
            else:
                self.assertEqual(process.returncode, 0, errors)

    def test_barrier_rejects_wrong_run_phase_and_duplicate_rank(self):
        for phase, run, duplicate in ((1, b"run-identity", False), (0, b"other-run", False), (0, b"run-identity", True)):
            with self.subTest(phase=phase, run=run, duplicate=duplicate):
                process = self.start(0)
                peers = []
                try:
                    for _ in range(2 if duplicate else 1):
                        deadline = time.monotonic() + 5
                        while True:
                            try:
                                peer = socket.create_connection(self.endpoint, timeout=1)
                                peers.append(peer)
                                break
                            except ConnectionRefusedError:
                                if time.monotonic() > deadline:
                                    raise
                                time.sleep(0.02)
                        peer.sendall(struct.pack("!4I", 1, phase, 1, 16) +
                                     run.ljust(128, b"\0") + b"build-identity".ljust(128, b"\0"))
                    output, errors = process.communicate(timeout=5)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertNotIn("_PASS ", output)
                    self.assertIn("mismatched run/build/rank/phase", errors)
                finally:
                    for peer in peers:
                        peer.close()


if __name__ == "__main__":
    unittest.main()
