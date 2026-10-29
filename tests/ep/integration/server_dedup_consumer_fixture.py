"""Shared build and process fixture for current Server Dedup consumer tests.

This module contains no test cases. Host substitutes do not prove NPU behavior.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import unittest
from uuid import NAMESPACE_URL, uuid5

ROOT = Path(__file__).resolve().parents[3]
SOURCE = Path(__file__).resolve().parent
CAPTURE_ID = str(uuid5(NAMESPACE_URL, "tilexr-validation:run-identity"))
COMBINE_MARKER = "EP_SERVER_DEDUP_COMBINE_CHAIN_PASS"

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


class ConsumerFixture:
    consumer = None
    compile_definitions = ["-DTILEXR_EP_SERVER_DEDUP_ENABLE=1", "-DTILEXR_EP_SERVER_DEDUP_AUTO_ENABLE=0"]

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="tilexr-consumer-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.folder = Path(cls.temp.name)
        if cls.consumer is not None:
            cls.executable = Path(cls.consumer).resolve()
            if not cls.executable.is_file() or not os.access(cls.executable, os.X_OK):
                raise RuntimeError(f"consumer is not executable: {cls.executable}")
            return
        compiler = shutil.which("g++")
        if compiler is None or os.name != "posix":
            raise unittest.SkipTest("POSIX C++14 compiler required")
        include = cls.folder / "include" / "acl"
        include.mkdir(parents=True)
        (include / "acl_base.h").write_text("#pragma once\ntypedef int aclError;\ntypedef void *aclrtStream;\n")
        (include / "acl.h").write_text(ACL_HEADER)
        cls.executable = cls.folder / "consumer"
        result = subprocess.run([
            compiler, "-std=c++14", "-O2", "-fno-fast-math", "-ffp-contract=off",
            "-Wall", "-Wextra", "-Werror", "-pthread", *cls.compile_definitions,
            "-I", str(include.parent), "-I", str(ROOT / "src/include"),
            "-I", str(ROOT / "src/comm"), "-I", str(ROOT / "src/ep/host"), "-I", str(ROOT / "src/ep/common"),
            str(SOURCE / "test_tilexr_ep_server_dedup_chain.cpp"),
            str(SOURCE / "server_dedup_chain_stub.cpp"),
            str(ROOT / "src/comm/tilexr_profiling_runtime.cpp"),
            str(ROOT / "src/ep/host/ep_memory_server_dedup_layout.cpp"),
            str(ROOT / "src/ep/host/ep_memory_server_dedup_combine_layout.cpp"),
            str(ROOT / "src/ep/host/ep_memory_layout.cpp"),
            str(ROOT / "src/ep/host/ep_layout.cpp"),
            "-o", str(cls.executable),
        ], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

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
        for key in ("OPERATOR_MODE", "DISPATCH_QUANT_MODE", "COMBINE_QUANT_MODE", "QUANT_MODE", "FP8_FORMAT"):
            self.environment.pop("TILEXR_VALIDATION_" + key, None)
        self.environment.pop("TILEXR_VALIDATION_ROUTE_MODE", None)
        self.environment.pop("TILEXR_VALIDATION_SEED", None)
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
            env.setdefault("TILEXR_PROFILING_COMBINE_RECORDS_PATH", str(Path(self.trace.name) / f"rank-{rank}.combine.records.v1"))
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

    def reset_case(self):
        self.stop()
        self.processes.clear()
        shutil.rmtree(self.trace.name)
        Path(self.trace.name).mkdir()

    def run_ranks(self, environment):
        return [self.start(rank, environment=environment) for rank in range(16)]

    def configure_combine(self):
        self.environment.update(TILEXR_VALIDATION_OPERATOR_MODE="both",
            TILEXR_VALIDATION_RUN_MODE="functional", TILEXR_VALIDATION_LOOPS="1",
            TILEXR_VALIDATION_WARMUP="0", TILEXR_VALIDATION_COMBINE_QUANT_MODE="0",
            TILEXR_VALIDATION_BS="16", TILEXR_VALIDATION_H="257", TILEXR_VALIDATION_TOPK="6")
        for key in tuple(self.environment):
            if key.startswith("TEST_") and key != "TEST_TRACE_DIR":
                self.environment.pop(key)


def run_tests(test_class):
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer")
    options, unittest_args = parser.parse_known_args()
    test_class.consumer = options.consumer
    program = unittest.main(argv=[__file__] + unittest_args, exit=False)
    if not program.result.wasSuccessful() or program.result.skipped:
        raise SystemExit(1)
