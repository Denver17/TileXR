#!/usr/bin/env python3
"""七步 EP 验证：构建、分发、每主机检查与有界结果汇总。"""

import argparse
import concurrent.futures
from contextlib import contextmanager
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import uuid
import measurements
import operator_measurements as operator_measure
import run_summary

from source import inventory


CHECKPOINT = "ep-sdedup-p1-abi-host"
COMBINE_ABI_HOST_CHECKPOINT = "ep-sdedup-combine-p1-abi-host"
COMBINE_ABI_HOST_TESTS = {
    "test_tilexr_ep_server_dedup_combine_" + name
    for name in ("public_abi", "layout", "host_validation")
}
COMBINE_EMPTY_KERNEL_CHECKPOINT = "ep-sdedup-combine-p1-empty-kernel"
COMBINE_REGISTRATION_TEST = "test_tilexr_ep_server_dedup_combine_registration"
COMBINE_LAUNCH_TEST = "test_tilexr_ep_server_dedup_combine_launch"
COMBINE_EMPTY_KERNEL_TESTS = COMBINE_ABI_HOST_TESTS | {COMBINE_LAUNCH_TEST, COMBINE_REGISTRATION_TEST}
COMBINE_DRY_RUN_CHECKPOINT = "ep-sdedup-combine-p1-dry-run"
COMBINE_DRY_RUN_TEST = "test_tilexr_ep_server_dedup_combine_dry_run"
COMBINE_DRY_RUN_PROGRAM = "test_tilexr_ep_server_dedup_chain_dry_run"
COMBINE_DRY_RUN_MARKER = "EP_SERVER_DEDUP_COMBINE_DRYRUN_PASS "
COMBINE_CHAIN_CHECKPOINT = "ep-sdedup-combine-p1-chain-single"
COMBINE_MULTI_CHECKPOINT = "ep-sdedup-combine-p1-chain-multi"
COMBINE_MULTI_TEST = "test_tilexr_ep_server_dedup_combine_multi"
COMBINE_PROFILE_CHECKPOINT = "ep-sdedup-combine-p1-profile"
COMBINE_PROFILE_TEST = "test_tilexr_ep_server_dedup_combine_profile"
COMBINE_PROFILE_KERNEL_TEST = "test_tilexr_ep_server_dedup_combine_profile_kernel"
COMBINE_PROFILE_LAUNCH_TEST = "test_tilexr_ep_server_dedup_combine_profile_launch"
COMBINE_PROFILE_REGIONS_TEST = "test_tilexr_ep_server_dedup_combine_profile_regions"
COMBINE_BOUNDARY_CHECKPOINT = "ep-sdedup-combine-p1-memory-boundary"
COMBINE_BOUNDARY_TEST = "test_tilexr_ep_server_dedup_combine_boundary_consumer"
COMBINE_BOUNDARY_LAUNCH_TEST = "test_tilexr_ep_server_dedup_combine_boundary_launch"
COMBINE_BOUNDARY_KERNEL_TEST = "test_tilexr_ep_server_dedup_combine_boundary_kernel"
COMBINE_BOUNDARY_MARKER = "EP_SERVER_DEDUP_COMBINE_BOUNDARY_PASS "
COMBINE_BOUNDARY_LIBRARY = "lib/libtilexr-ep-combine-memory-boundary-test.so"
COMBINE_BOUNDARY_KERNEL = "lib/tilexr/kernels/tilexr_ep_combine_memory_boundary_test_kernel.o"
COMBINE_M12_CHECKPOINT = "ep-sdedup-combine-p2-m12"
COMBINE_M3_CHECKPOINT = "ep-sdedup-combine-p2-m3"
COMBINE_MODULE_CHECKPOINTS = (COMBINE_M12_CHECKPOINT, COMBINE_M3_CHECKPOINT)
COMBINE_M3_TEST = "test_tilexr_ep_server_dedup_combine_m3"
COMBINE_M3_CONSUMER_TEST = "test_tilexr_ep_server_dedup_combine_m3_consumer"
COMBINE_INVALID_MARKER = "EP_SERVER_DEDUP_COMBINE_CHAIN_INVALID "
COMBINE_M12_TEST = "test_tilexr_ep_server_dedup_combine_m12"
COMBINE_M12_CONSUMER_TEST = "test_tilexr_ep_server_dedup_combine_m12_consumer"
COMBINE_CONFIG_TEST = "test_tilexr_ep_server_dedup_combine_config"
COMBINE_MEASUREMENT_CHECKPOINTS = (COMBINE_PROFILE_CHECKPOINT, *COMBINE_MODULE_CHECKPOINTS)
COMBINE_CONTINUOUS_CHECKPOINTS = (COMBINE_MULTI_CHECKPOINT, COMBINE_PROFILE_CHECKPOINT, COMBINE_BOUNDARY_CHECKPOINT,
                                  *COMBINE_MODULE_CHECKPOINTS)
COMBINE_DEVICE_CHECKPOINTS = (COMBINE_CHAIN_CHECKPOINT, *COMBINE_CONTINUOUS_CHECKPOINTS)
COMBINE_CHAIN_TEST = "test_tilexr_ep_server_dedup_combine_chain"
COMBINE_DELAY_TEST = "test_tilexr_ep_server_dedup_combine_delay"
COMBINE_CHAIN_MARKER = "EP_SERVER_DEDUP_COMBINE_CHAIN_PASS "
COMBINE_TESTS = {
    COMBINE_ABI_HOST_CHECKPOINT: COMBINE_ABI_HOST_TESTS,
    COMBINE_EMPTY_KERNEL_CHECKPOINT: COMBINE_EMPTY_KERNEL_TESTS,
    COMBINE_DRY_RUN_CHECKPOINT: COMBINE_ABI_HOST_TESTS | {COMBINE_DRY_RUN_TEST},
    COMBINE_CHAIN_CHECKPOINT: COMBINE_ABI_HOST_TESTS | {COMBINE_DRY_RUN_TEST, COMBINE_CHAIN_TEST, COMBINE_DELAY_TEST},
    COMBINE_MULTI_CHECKPOINT: COMBINE_ABI_HOST_TESTS | {COMBINE_DRY_RUN_TEST, COMBINE_CHAIN_TEST, COMBINE_DELAY_TEST, COMBINE_MULTI_TEST},
}
COMBINE_TESTS[COMBINE_PROFILE_CHECKPOINT] = COMBINE_TESTS[COMBINE_MULTI_CHECKPOINT] | {COMBINE_PROFILE_TEST, COMBINE_PROFILE_REGIONS_TEST, COMBINE_PROFILE_LAUNCH_TEST, COMBINE_PROFILE_KERNEL_TEST}
COMBINE_TESTS[COMBINE_BOUNDARY_CHECKPOINT] = COMBINE_TESTS[COMBINE_PROFILE_CHECKPOINT] | {
    COMBINE_BOUNDARY_TEST, COMBINE_BOUNDARY_LAUNCH_TEST, COMBINE_BOUNDARY_KERNEL_TEST}
COMBINE_TESTS[COMBINE_M12_CHECKPOINT] = COMBINE_TESTS[COMBINE_PROFILE_CHECKPOINT] | {COMBINE_M12_TEST, COMBINE_M12_CONSUMER_TEST, COMBINE_CONFIG_TEST}
COMBINE_TESTS[COMBINE_M3_CHECKPOINT] = COMBINE_ABI_HOST_TESTS | {
    COMBINE_DRY_RUN_TEST, COMBINE_PROFILE_REGIONS_TEST, COMBINE_PROFILE_LAUNCH_TEST,
    COMBINE_PROFILE_KERNEL_TEST, COMBINE_CONFIG_TEST, COMBINE_M3_TEST, COMBINE_M3_CONSUMER_TEST}
COMBINE_ABI = {"launch_bytes": 88, "tiling_bytes": 16, "runtime_args_bytes": 88,
               "block_dim": 64, "kernel_scope": "combine-empty"}
COMBINE_M12_ABI = dict(COMBINE_ABI, kernel_scope="combine-m12", implemented_through="M2",
                       control_bytes=96 * 1024, send_core_num=32)
COMBINE_M3_ABI = dict(COMBINE_M12_ABI, kernel_scope="combine-m3", implemented_through="M3", delay_us=0)


def combine_contract(checkpoint):
    return {COMBINE_M12_CHECKPOINT: COMBINE_M12_ABI, COMBINE_M3_CHECKPOINT: COMBINE_M3_ABI}.get(checkpoint, COMBINE_ABI)


EMPTY_KERNEL_CHECKPOINT = "ep-sdedup-p1-empty-kernel"
DRY_RUN_CHECKPOINT = "ep-sdedup-p1-dry-run"
DRY_RUN_MARKER = "EP_SERVER_DEDUP_DRYRUN_PASS "
CHAIN_CHECKPOINT = "ep-sdedup-p1-chain-single"
CHAIN_MULTI_CHECKPOINT = "ep-sdedup-p1-chain-multi"
DISPATCH_CHECKPOINT = "ep-sdedup-p2-dispatch"
DISPATCH_MARKER = "EP_SERVER_DEDUP_DISPATCH_PASS "
DISPATCH_INVALID_MARKER = "EP_SERVER_DEDUP_DISPATCH_INVALID "
DISPATCH_ABI = {"launch_bytes": 120, "tiling_bytes": 32, "runtime_args_bytes": 120,
                "layout": "r12-shared-peer-buffer", "kernel_scope": "dispatch"}
MEMORY_BOUNDARY_CHECKPOINT = "ep-sdedup-p1-memory-boundary"
MEMORY_BOUNDARY_MARKER = "EP_SERVER_DEDUP_MEMORY_BOUNDARY_PASS "
SERVER_DEDUP_PEER_BYTES = 8 * 1024 ** 3
MEMORY_BOUNDARY_BYTES = 4 * 1024 ** 3
CHAIN_CHECKPOINTS = (DISPATCH_CHECKPOINT, MEMORY_BOUNDARY_CHECKPOINT, *COMBINE_DEVICE_CHECKPOINTS)
CHAIN_PROGRAM = "test_tilexr_ep_server_dedup_chain"
CHAIN_MARKER = "EP_SERVER_DEDUP_CHAIN_PASS "
BREAKDOWN_MARKER = "EP_SERVER_DEDUP_BREAKDOWN_PASS "
OPERATOR_BREAKDOWN_MARKER = "EP_SERVER_DEDUP_OPERATOR_BREAKDOWN_PASS "
RANK_CHECKPOINTS = (DRY_RUN_CHECKPOINT, *CHAIN_CHECKPOINTS)
REGISTRATION_TEST = "test_tilexr_ep_server_dedup_registration"
CHECKS = ("protocol", "layout", "host_validation", "public_abi")
REQUIRED_TESTS = {f"test_tilexr_ep_server_dedup_{name}" for name in CHECKS} | {
    "test_tilexr_ep_server_dedup_counts", "test_tilexr_ep_server_dedup_source_send",
    "test_tilexr_ep_server_dedup_receive_packet",
    "test_dispatch_memory_trace", "test_msprof_kernel_stats", "test_tilexr_ep_layout",
    "test_tilexr_ep_expert_ids", "test_tilexr_ep_host_validation", "test_tilexr_ep_kernel_launch",
    "test_tilexr_moonep_planner_public_abi", "test_tilexr_ep_plan_layout", "test_tilexr_ep_plan_reference",
    "test_tilexr_ep_plan_algorithm", "test_tilexr_ep_plan_downstream", "test_tilexr_ep_plan_host",
    "test_tilexr_moonep_planner_compat",
}
FAILURE_LABELS = {
    "CONFIG_FAILED": "配置或安装包检查失败", "BUILD_FAILED": "编译或安装失败",
    "SYNC_FAILED": "同步失败", "LAUNCH_FAILED": "进程拉起失败", "EXIT_FAILED": "进程退出失败",
    "RESULT_FAILED": "结果校验失败", "LOG_PARSE_FAILED": "日志解析失败",
    "LOG_COLLECT_FAILED": "日志收集失败", "CLEANUP_FAILED": "进程清理失败",
}
FAILURE_CODES = {name: code for code, name in enumerate(FAILURE_LABELS, 10)}
SSH_OPTIONS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
               "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"]
REMOTE_SCRIPT = '''set -eo pipefail
if [ "$2" = cancel ]; then
    if [ -n "${TILEXR_DIRECT_CONFIG:-}" ]; then eval "${TILEXR_DIRECT_CONFIG}"; fi
    cd -- "$1"
    shift
    exec python3 tools/validation/workflow.py "$@"
fi
limit=60
if [ "$2" = node ]; then limit=$5; fi
exec timeout --signal=TERM --kill-after=10s "$limit" bash -c '
set -eo pipefail
cd -- "$1"
shift
export TILEXR_VALIDATION_ACTION="$1"
source ./set_env.sh
if [ -n "${TILEXR_DIRECT_CONFIG:-}" ]; then
    eval "${TILEXR_DIRECT_CONFIG}"
fi
exec "$TILEXR_VALIDATION_PYTHON" tools/validation/workflow.py "$@"
' bash "$@"
'''


class Failure(RuntimeError):
    def __init__(self, kind, message, **details):
        super().__init__(message)
        self.kind = kind
        self.details = details

    def record(self, **context):
        return {"type": self.kind, "message": str(self), **self.details, **context}


def failure_record(error, default, **context):
    if isinstance(error, Failure):
        return error.record(**context)
    return Failure(default, str(error)).record(**context)


def failure_records(error, default, **context):
    record = failure_record(error, default, **context)
    additional = record.pop("additional_failures", [])
    return [record, *additional]


def report_failure(record):
    print(f"VALIDATION_FAIL type={record['type']} {FAILURE_LABELS[record['type']]}："
          + json.dumps(record, ensure_ascii=False), file=sys.stderr, flush=True)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def identifier(value):
    if not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", value):
        raise ValueError("无效的运行标识")
    return value


def stop_process(process):
    # 所有命令拥有独立进程组。只清理本次创建的组，不按程序名称杀进程。
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.wait()
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def execute(argv, log, timeout=180, cwd=None, env=None, stdin=None,
            start_failure="LAUNCH_FAILED", exit_failure="EXIT_FAILED", parse_failure="LOG_PARSE_FAILED",
            cancelled=None):
    """写原始日志；超时和异常退出均回收本次命令及其同组子进程。"""
    log = Path(log)
    log.parent.mkdir(parents=True, exist_ok=True)
    print(f"执行：{argv[0]}  日志：{log}", flush=True)
    with log.open("wb") as output:
        try:
            process = subprocess.Popen([str(arg) for arg in argv], cwd=cwd, env=env,
                                       stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                                       stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        except OSError as error:
            raise Failure(start_failure, f"无法启动 {argv[0]}：{error}", log=str(log)) from error
        pending = None
        try:
            if cancelled is None:
                process.communicate(input=stdin.encode() if stdin is not None else None, timeout=timeout)
            else:
                # 并行 rank 共用取消信号；仍由本次 execute 清理自己创建的进程组。
                deadline = time.monotonic() + timeout
                while True:
                    if cancelled.is_set():
                        raise InterruptedError("本次 rank 运行已取消")
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise subprocess.TimeoutExpired(argv, timeout)
                    try:
                        process.communicate(timeout=min(0.1, remaining))
                        break
                    except subprocess.TimeoutExpired:
                        continue
            if process.returncode:
                pending = Failure(exit_failure, f"命令失败（exit={process.returncode}）：{argv[0]}",
                                  returncode=process.returncode, log=str(log))
        except subprocess.TimeoutExpired:
            pending = Failure(exit_failure, f"命令超过 {timeout:.1f} 秒期限：{argv[0]}",
                              timeout=True, log=str(log))
        except InterruptedError as error:
            pending = Failure(exit_failure, str(error), log=str(log))
        finally:
            try:
                stop_process(process)
            except Exception as error:
                cleanup = Failure("CLEANUP_FAILED", str(error), log=str(log))
                if pending is not None:
                    pending.details["additional_failures"] = [cleanup.record()]
                    raise pending from error
                raise cleanup from error
    if pending is not None:
        tail = log.read_text(encoding="utf-8", errors="replace")[-4000:]
        pending.args = (str(pending) + f"\n日志：{log}\n" + tail,)
        raise pending
    try:
        return log.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise Failure(parse_failure, f"无法读取 UTF-8 日志：{error}", log=str(log)) from error


def hosts_from_file(path):
    hosts = []
    for number, line in enumerate(Path(path).read_text(encoding="utf-8-sig").splitlines(), 1):
        fields = line.split("#", 1)[0].split()
        if not fields:
            continue
        try:
            if len(fields) != 1:
                raise ValueError()
            ipaddress.IPv4Address(fields[0])
        except ValueError:
            raise ValueError(f"hostfile 第 {number} 行格式错误：每行只填写一个 IPv4，整机使用 device 0..7") from None
        if fields[0] in [item["host"] for item in hosts]:
            raise ValueError(f"hostfile 主机重复：{fields[0]}")
        hosts.append({"host": fields[0], "devices": list(range(8))})
    if not 1 <= len(hosts) <= 64:
        raise ValueError("hostfile 必须包含 1 至 64 台主机")
    return hosts


def hosts_from_json(raw):
    try:
        hosts = json.loads(raw)
    except (TypeError, json.JSONDecodeError) as error:
        raise ValueError("直接运行的 hostfile 配置不是有效 JSON") from error
    if not isinstance(hosts, list):
        raise ValueError("直接运行的 hostfile 配置必须是数组")
    for item in hosts:
        if (not isinstance(item, dict) or not isinstance(item.get("host"), str)
                or item.get("devices") != list(range(8))):
            raise ValueError("直接运行的 hostfile 配置无效")
        ipaddress.IPv4Address(item["host"])
    if not 1 <= len(hosts) <= 64 or len({item["host"] for item in hosts}) != len(hosts):
        raise ValueError("直接运行的 hostfile 主机数量或主机重复")
    return hosts


class Config:
    def __init__(self, action="run"):
        self.action = action
        self.is_run = action in ("run", "node", "rank", "case-dry-run")
        self.root = Path(os.environ.get("TILEXR_VALIDATION_ROOT", Path(__file__).resolve().parents[2])).resolve()
        self.mode = os.environ.get("TILEXR_VALIDATION_BUILD_MODE", "production")
        # 同步和人工测试继承安装检查点；build_all.sh 选择当前生产增量。
        self.checkpoint = os.environ.get("TILEXR_VALIDATION_CHECKPOINT")
        if not self.checkpoint:
            installed = self.root / "install/validation/manifest.json"
            self.checkpoint = read_json(installed)["checkpoint"] if installed.exists() else CHECKPOINT
        local_only = action in ("build", "verify", "host-validate", "commit-validate") and self.checkpoint not in COMBINE_DEVICE_CHECKPOINTS
        self.hostfile = (self.root / "hostfile" if local_only else
                         Path(os.environ.get("TILEXR_VALIDATION_HOSTFILE", self.root / "hostfile")))
        self.user = "" if local_only else os.environ.get("TILEXR_VALIDATION_SSH_USER", "")
        self.jobs = int(os.environ.get("TILEXR_VALIDATION_JOBS", "4")) if action in ("build", "commit-validate") else 4
        self.timeout = int(os.environ.get("TILEXR_VALIDATION_TIMEOUT", "180")) if self.is_run else 180
        self.server_count = None
        if self.is_run and os.environ.get("TILEXR_VALIDATION_SERVERS") is not None:
            self.server_count = int(os.environ["TILEXR_VALIDATION_SERVERS"])
            if not 2 <= self.server_count <= 16:
                raise ValueError("-n 必须为 2..16 台服务器")
        self.profiling = False
        if action == "build":
            breakdown = os.environ.get("TILEXR_VALIDATION_BREAKDOWN", "0")
            if breakdown not in ("0", "1"):
                raise ValueError("编译 breakdown 必须为 0/1")
            self.profiling = breakdown == "1"
        if self.checkpoint == COMBINE_BOUNDARY_CHECKPOINT and self.profiling:
            raise ValueError("Combine C7要求profiling OFF产物")
        probe = os.environ.get("TILEXR_VALIDATION_BOUNDARY_PROBE", "0")
        if probe not in ("0", "1") or (probe == "1" and self.checkpoint != COMBINE_BOUNDARY_CHECKPOINT):
            raise ValueError("BOUNDARY_PROBE必须为0/1，且仅C7允许启用")
        if self.checkpoint in (CHAIN_CHECKPOINT, CHAIN_MULTI_CHECKPOINT, "ep-sdedup-p2-first-hop", "ep-sdedup-p2-source-send"):
            raise ValueError("旧 cnt-only/first-hop/source-send checkpoint 已由 ep-sdedup-p2-dispatch 替代；更新 CHECKPOINT 后执行 build_all.sh 和 sync.sh")
        if self.mode not in ("host", "production") or self.checkpoint not in (
                CHECKPOINT, *COMBINE_TESTS, EMPTY_KERNEL_CHECKPOINT, *RANK_CHECKPOINTS):
            raise ValueError("不支持的 checkpoint：" + self.checkpoint)
        if self.checkpoint in (COMBINE_ABI_HOST_CHECKPOINT, COMBINE_DRY_RUN_CHECKPOINT) and self.mode != "host":
            raise ValueError("Combine C1/C3 仅验证 Host ABI/layout/configuration；请使用 host 模式")
        if action == "commit-validate" and self.checkpoint not in COMBINE_TESTS:
            raise ValueError("当前提交验收入口仅支持已定义的 Combine checkpoint")
        if self.checkpoint in (EMPTY_KERNEL_CHECKPOINT, COMBINE_EMPTY_KERNEL_CHECKPOINT, *CHAIN_CHECKPOINTS) and self.mode != "production":
            raise ValueError("empty-kernel / dispatch / memory-boundary 必须使用 production 模式验证真实 A5 artifact 和设备调用")
        if not 1 <= self.jobs <= 256 or not 30 <= self.timeout <= 180:
            raise ValueError("JOBS 必须为 1..256，TIMEOUT 必须为 30..180 秒")
        if self.user and not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", self.user):
            raise ValueError("SSH_USER 格式错误")
        self.hostfile = self.hostfile.resolve()
        self.hostfile.relative_to(self.root)  # 同步契约：配置属于当前工作目录。
        self.install = self.root / "install" / validation_directory(self.checkpoint)
        self.logs = Path(os.environ.get("TILEXR_VALIDATION_RESULT_ROOT", self.root / "run/validation")).resolve()
        self.chain = None
        if self.is_run and self.checkpoint in (*CHAIN_CHECKPOINTS, COMBINE_DRY_RUN_CHECKPOINT):
            default_host = None
            if self.checkpoint != COMBINE_DRY_RUN_CHECKPOINT and not os.environ.get("TILEXR_COMM_ID"):
                default_host = self.hosts()[0]["host"]
            self.chain = chain_configuration(self.checkpoint, default_host)
        if self.checkpoint in COMBINE_MODULE_CHECKPOINTS:
            profile_on = self.profiling if action == "build" else (
                self.chain["run_mode"] == "breakdown" if self.chain is not None else
                os.environ.get("TILEXR_VALIDATION_BREAKDOWN", "0") == "1")
            self.install = self.install.with_name(self.install.name + ("-profile-on" if profile_on else "-profile-off"))
        if action == "case-dry-run" and self.checkpoint != COMBINE_DRY_RUN_CHECKPOINT:
            raise ValueError("case-dry-run 要求 Combine C3 checkpoint")
        if action in ("run", "node", "rank") and self.checkpoint == COMBINE_DRY_RUN_CHECKPOINT:
            raise ValueError("Combine C3 仅支持 case-dry-run，不允许实际设备执行")

    def hosts(self):
        try:
            hosts = (hosts_from_json(os.environ["TILEXR_DIRECT_HOSTS_JSON"])
                     if self.is_run and os.environ.get("TILEXR_DIRECT_HOSTS_JSON")
                     else hosts_from_file(self.hostfile))
            if self.server_count is not None:
                if self.server_count > len(hosts):
                    raise ValueError(f"-n {self.server_count} 超过 hostfile 中的 {len(hosts)} 台服务器")
                hosts = hosts[:self.server_count]
            if self.is_run and self.checkpoint in RANK_CHECKPOINTS:
                rank_mapping(hosts)
            if self.chain is not None and self.checkpoint != MEMORY_BOUNDARY_CHECKPOINT:
                ranks = len(hosts) * 8
                experts = self.chain["moe_expert_num"]
                if not 0 < experts <= 1024 or experts % ranks or self.chain["topk"] > experts:
                    raise ValueError("chain 检查点要求 0<moe_expert_num<=1024，能被 rank_size 整除，topk<=moe_expert_num")
                epoch = experts // math.gcd(experts, self.chain["topk"])
                if (self.chain["operator_mode"] != "combine" and self.chain["route_mode"] == "balance_shift"
                        and self.chain["bs"] * ranks % epoch):
                    raise ValueError("balance_shift 要求全局 Token 数能被 E/gcd(E,TopK) 整除")
            return hosts
        except FileNotFoundError:
            raise ValueError(f"hostfile 不存在：{self.hostfile}；请按 hostfile.example 一次性配置主机清单，"
                             "或检查 validation.local.sh 中的 TILEXR_VALIDATION_HOSTFILE。") from None

    def target(self, host):
        return f"{self.user}@{host}" if self.user else host


def source_send_stride(config):
    h, k = config["h"], config["topk"]
    data = ((h + 255) // 256 * 256 + (h + 63) // 64 * 2) if config["quant_mode"] == 4 else h * 2
    index = (data + 31) // 32 * 32 + 32
    if index % 480 + 8 * k > 480:
        index = (index + 479) // 480 * 480
    payload = index + 8 * k
    return (payload + 479) // 480 * 512


def chain_configuration(checkpoint, default_host=None):
    route_mode = os.environ.get("TILEXR_VALIDATION_ROUTE_MODE", "balance_shift")
    if route_mode not in ("random", "balance", "balance_shift"):
        raise ValueError("route_mode 必须为 random、balance 或 balance_shift")
    seed = os.environ.get("TILEXR_VALIDATION_SEED", "2026")
    if not re.fullmatch(r"0|[1-9][0-9]{0,9}", seed) or int(seed) > (1 << 32) - 1:
        raise ValueError("seed 必须为 0..4294967295 的整数")
    case_analysis = os.environ.get("TILEXR_VALIDATION_CASE_ANALYSIS", "0")
    if case_analysis not in ("0", "1"):
        raise ValueError("case_analysis 必须为 0 或 1")
    if case_analysis == "1" and checkpoint != DISPATCH_CHECKPOINT:
        raise ValueError("case_analysis 只支持完整 Dispatch 用例")
    if checkpoint in (MEMORY_BOUNDARY_CHECKPOINT, COMBINE_CHAIN_CHECKPOINT) and (
            os.environ.get("TILEXR_VALIDATION_RUN_MODE", "functional") != "functional" or
            os.environ.get("TILEXR_VALIDATION_LOOPS", "1") != "1" or
            os.environ.get("TILEXR_VALIDATION_WARMUP", "0") != "0"):
        raise ValueError("memory-boundary / Combine C4 仅执行 functional，loops=1，warmup=0")
    if checkpoint in (COMBINE_MULTI_CHECKPOINT, COMBINE_BOUNDARY_CHECKPOINT) and os.environ.get("TILEXR_VALIDATION_RUN_MODE", "functional") != "functional":
        raise ValueError("Combine C5/C7 仅执行 functional；performance/breakdown 请选择 C6")
    shape = {name: int(os.environ.get("TILEXR_VALIDATION_" + name.upper(), default))
             for name, default in (("bs", "96"), ("h", "7168"), ("topk", "6"), ("moe_expert_num", "96"))}
    if any(not 1 <= value <= (1 << 63) - 1 for value in shape.values()) or shape["topk"] > 32:
        raise ValueError("chain 检查点 shape 必须是正 int64，topk<=32")
    dtype = os.environ.get("TILEXR_VALIDATION_DTYPE", "bf16")
    operator_mode = os.environ.get("TILEXR_VALIDATION_OPERATOR_MODE", "dispatch")
    if operator_mode not in ("dispatch", "combine", "both"):
        raise ValueError("OPERATOR_MODE 必须为 dispatch/combine/both")
    if operator_mode != "dispatch" and checkpoint not in (COMBINE_DRY_RUN_CHECKPOINT, *COMBINE_DEVICE_CHECKPOINTS):
        raise ValueError("Combine/both 真实执行要求 C4/C5 checkpoint；其他检查点仅支持 dry_run=1")
    legacy_quant = os.environ.get("TILEXR_VALIDATION_QUANT_MODE")
    dispatch_quant = os.environ.get("TILEXR_VALIDATION_DISPATCH_QUANT_MODE")
    if legacy_quant is not None and dispatch_quant is not None and legacy_quant != dispatch_quant:
        raise ValueError("QUANT_MODE 与 DISPATCH_QUANT_MODE 冲突")
    quant_mode = dispatch_quant if dispatch_quant is not None else legacy_quant if legacy_quant is not None else "0"
    combine_quant_mode = os.environ.get("TILEXR_VALIDATION_COMBINE_QUANT_MODE", "0")
    if combine_quant_mode != "0":
        raise ValueError("COMBINE_QUANT_MODE 目前仅支持 0（非量化）")
    if quant_mode not in ("0", "4"):
        raise ValueError("QUANT_MODE 必须为 0（非量化）或 4（MXFP8），FP8_FORMAT 选择 e4m3/e5m2")
    fp8_format = os.environ.get("TILEXR_VALIDATION_FP8_FORMAT", "e4m3")
    if fp8_format not in ("e4m3", "e5m2"):
        raise ValueError("FP8_FORMAT 必须为 e4m3/e5m2")
    block_dim = os.environ.get("TILEXR_VALIDATION_BLOCK_DIM", "auto")
    if dtype not in ("bf16", "fp16") or (block_dim != "auto" and
            (not block_dim.isdecimal() or not 1 <= int(block_dim) <= (1 << 32) - 1)):
        raise ValueError("chain 检查点 dtype 必须为 bf16/fp16，block_dim 必须为 auto 或正 uint32")
    run_mode, loops, warmup = "functional", 1, 0
    if checkpoint in (DISPATCH_CHECKPOINT, COMBINE_DRY_RUN_CHECKPOINT, *COMBINE_DEVICE_CHECKPOINTS):
        run_mode = os.environ.get("TILEXR_VALIDATION_RUN_MODE", "functional")
        if run_mode not in ("functional", "performance", "breakdown"):
            raise ValueError("RUN_MODE 必须为 functional/performance/breakdown")
        loops = int(os.environ.get("TILEXR_VALIDATION_LOOPS", "1"))
        warmup = int(os.environ.get("TILEXR_VALIDATION_WARMUP", "0"))
        if not 1 <= loops <= (1 << 32) - 1 or not 0 <= warmup <= (1 << 32) - 1:
            raise ValueError("loops 必须为正 uint32，warmup 必须为非负 uint32")
        if run_mode == "breakdown" and checkpoint != COMBINE_DRY_RUN_CHECKPOINT:
            measurements.capture_capacity(loops, warmup)
        if block_dim not in ("auto", "64"):
            raise ValueError("Dispatch 总核数固定为 64，block_dim 必须为 auto/64")

    def endpoint(value, name):
        try:
            host, port = value.split(":")
            address = ipaddress.IPv4Address(host)
            if address.is_loopback or address.is_unspecified or address.is_multicast or not 1 <= int(port) <= 65535:
                raise ValueError()
            return f"{address}:{int(port)}"
        except ValueError:
            raise ValueError(f"{name} 必须为首节点可跨机访问的IPv4:端口；检查 hostfile 首项或 run.sh 地址覆盖参数") from None

    boundary_probe = os.environ.get("TILEXR_VALIDATION_BOUNDARY_PROBE", "0") == "1"
    if boundary_probe and (checkpoint != COMBINE_BOUNDARY_CHECKPOINT or loops != 1 or warmup != 0):
        raise ValueError("C7边界探针要求loops=1、warmup=0")
    chain = dict(shape, dtype=dtype, operator_mode=operator_mode,
                 quant_mode=int(quant_mode), dispatch_quant_mode=int(quant_mode), combine_quant_mode=0,
                 fp8_format=fp8_format, block_dim=block_dim, run_mode=run_mode, loops=loops, warmup=warmup,
                 route_mode=route_mode, seed=int(seed), case_analysis=int(case_analysis))
    if checkpoint == COMBINE_BOUNDARY_CHECKPOINT:
        chain["boundary_probe"] = boundary_probe
    if checkpoint in COMBINE_MODULE_CHECKPOINTS:
        fixture = os.environ.get("TILEXR_VALIDATION_COMBINE_FIXTURE", "balanced")
        observation = os.environ.get("TILEXR_VALIDATION_COMBINE_OBSERVATION", "batch")
        if fixture not in ("balanced", "sparse", "empty") or (fixture != "balanced" and operator_mode != "combine"):
            raise ValueError("M1/M2 fixture必须为balanced/sparse/empty；非balanced仅支持combine独跑")
        if observation not in ("batch", "per-round") or (observation != "batch" and run_mode != "functional"):
            raise ValueError("M1/M2逐轮观察仅支持functional")
        if checkpoint == COMBINE_M3_CHECKPOINT and (loops != 1 or warmup != 0 or observation != "batch" or fixture != "balanced"):
            raise ValueError("M3 requires one launch, zero warmup, balanced fixture and batch observation in every mode")
        chain.update(combine_fixture=fixture, combine_observation=observation,
                     implemented_through="M3" if checkpoint == COMBINE_M3_CHECKPOINT else "M2",
                     scope="partial", combine_send_core_num=32)
    if checkpoint == COMBINE_DRY_RUN_CHECKPOINT:
        return chain

    comm_id = endpoint(os.environ.get("TILEXR_COMM_ID") or f"{default_host}:10177", "TILEXR_COMM_ID")
    host, port = comm_id.split(":")
    barrier_addr = endpoint(os.environ.get("TILEXR_DEMO_BARRIER_ADDR", f"{host}:{int(port) + 97}"),
                            "TILEXR_DEMO_BARRIER_ADDR")
    if comm_id == barrier_addr:
        raise ValueError("communicator 与 Host barrier 必须使用不同端口")
    scope = {"dispatch": "dispatch", "combine": "combine-empty", "both": "dispatch-combine-empty"}[operator_mode]
    if checkpoint in COMBINE_MODULE_CHECKPOINTS and operator_mode != "dispatch":
        module = "m3" if checkpoint == COMBINE_M3_CHECKPOINT else "m12"
        scope = ("dispatch-combine-" if operator_mode == "both" else "combine-") + module
    chain.update(comm_id=comm_id, barrier_addr=barrier_addr, kernel_scope="memory-boundary" if boundary_probe else scope)
    if run_mode == "performance":
        chain["msprof"] = os.environ.get("TILEXR_VALIDATION_MSPROF", "msprof")
        if not chain["msprof"]:
            raise ValueError("TILEXR_VALIDATION_MSPROF 不能为空")
    return chain


@contextmanager
def operation_lock(config, name):
    config.logs.mkdir(parents=True, exist_ok=True)
    lock = config.logs / name
    if name == ".operation.lock" and (config.logs / "node.lock").exists():
        raise ValueError("本目录仍有节点测试运行或清理未确认，不能开始新的操作")
    try:
        lock.mkdir()
    except FileExistsError:
        raise ValueError(f"该目录已有验证任务或未确认清理的锁：{lock}")
    try:
        yield
    except Failure as error:
        if any(record["type"] == "CLEANUP_FAILED" for record in failure_records(error, "CLEANUP_FAILED")):
            config.cleanup_failed = True
        raise
    finally:
        if not getattr(config, "cleanup_failed", False):
            lock.rmdir()


def check_coordinator(config, hosts):
    # 清单顺序是节点编号来源；首项在当前进程执行，其余项通过 SSH 执行。
    host = hosts[0]["host"]
    local_names = {"localhost", "127.0.0.1", socket.gethostname(), socket.getfqdn()}
    if host in local_names:
        return
    addresses = set(subprocess.check_output(["hostname", "-I"], text=True).split())
    try:
        addresses_for_host = {entry[4][0] for entry in socket.getaddrinfo(host, None)}
    except socket.gaierror:
        addresses_for_host = set()
    if not addresses.intersection(addresses_for_host):
        # SSH config 别名也允许；只读探测确认首项确实指向当前节点。
        actual = execute(["ssh", *SSH_OPTIONS, config.target(host), "hostname"],
                         config.logs / "coordinator.log", timeout=20).strip()
        if actual != socket.gethostname():
            raise ValueError("hostfile 首项必须是当前执行脚本的服务器")


def require_tools(names):
    for name in names:
        if shutil.which(name) is None:
            raise ValueError(f"缺少依赖：{name}")


def validation_directory(checkpoint):
    return {COMBINE_ABI_HOST_CHECKPOINT: "validation-combine-c1",
            COMBINE_EMPTY_KERNEL_CHECKPOINT: "validation-combine-c2",
            COMBINE_DRY_RUN_CHECKPOINT: "validation-combine-c3",
            COMBINE_CHAIN_CHECKPOINT: "validation-combine-c4",
            COMBINE_MULTI_CHECKPOINT: "validation-combine-c5",
            COMBINE_PROFILE_CHECKPOINT: "validation-combine-c6",
            COMBINE_BOUNDARY_CHECKPOINT: "validation-combine-c7",
            COMBINE_M12_CHECKPOINT: "validation-combine-p21", COMBINE_M3_CHECKPOINT: "validation-combine-p22"}.get(checkpoint, "validation")


def runtime_check(config, install, log):
    if config.mode == "host":
        return
    require_tools(["ldd", "readelf", "nm"])
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(install / "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
    library = install / "lib/libtilexr-ep.so"

    def inspect(argv, filename, env=None):
        deadline = getattr(config, "build_deadline", None)
        budget = min(30, deadline - time.monotonic()) if deadline is not None else 30
        if budget <= 0:
            raise Failure("BUILD_FAILED", "Combine 构建超过 900 秒总期限", timeout=True)
        return execute(argv, log / filename, timeout=budget, env=env,
                       start_failure="BUILD_FAILED", exit_failure="BUILD_FAILED")

    if config.checkpoint == EMPTY_KERNEL_CHECKPOINT:
        kernel = install / "lib/tilexr/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.o"
        inspect(["readelf", "-h", kernel], "server-dedup-elf.log")

    if config.checkpoint in (COMBINE_EMPTY_KERNEL_CHECKPOINT, *COMBINE_DEVICE_CHECKPOINTS):
        kernel = install / "lib/tilexr/kernels/tilexr_ep_combine_memory_server_dedup_kernel.o"
        inspect(["readelf", "-h", kernel], "combine-server-dedup-elf.log")

    if config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT:
        inspect(["readelf", "-h", install / COMBINE_BOUNDARY_KERNEL], "combine-boundary-elf.log")
        probe_dependencies = inspect(["ldd", "-r", install / COMBINE_BOUNDARY_LIBRARY],
                                     "combine-boundary-dependencies.log", env=env)
        if any(word in probe_dependencies for word in ("not found", "undefined symbol", "/devlib/")):
            raise ValueError("C7边界探针依赖未正确解析")
    symbols = inspect(["nm", "-D", "--defined-only", "--no-demangle", "--format=posix", library],
                      "symbols.log")
    # 公共接口采用 extern "C"；匹配完整动态函数符号名，不带参数列表或左括号。
    exported = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in ("T", "W"):
            exported.add(fields[0])
    required_symbols = ["TileXRMoeEpDispatchMemoryServerDedup", "TileXRMoeEpDispatchMemory"]
    if config.checkpoint in (COMBINE_EMPTY_KERNEL_CHECKPOINT, *COMBINE_DEVICE_CHECKPOINTS):
        required_symbols.append("TileXRMoeEpCombineMemoryServerDedup")
    for name in required_symbols:
        if name not in exported:
            raise ValueError(f"生产库缺少导出符号：{name}；库：{library}；日志：{log / 'symbols.log'}")
    elf = inspect(["readelf", "-d", library], "elf.log")
    paths = [line for line in elf.splitlines() if "(RPATH)" in line or "(RUNPATH)" in line]
    if len(paths) != 1 or "[$ORIGIN]" not in paths[0]:
        raise ValueError("libtilexr-ep.so 必须以 $ORIGIN 查找相邻库，不得加载 CANN devlib")
    dependencies = inspect(["ldd", "-r", library], "dependencies.log", env=env)
    if any(word in dependencies for word in ("not found", "undefined symbol", "/devlib/")):
        raise ValueError(f"生产库依赖未正确解析：{log / 'dependencies.log'}")
    hal = re.search(r"libascend_hal\.so\S*\s+=>\s+(\S+)", dependencies)
    driver = Path(os.environ.get("ASCEND_DRIVER_PATH", "/usr/local/Ascend/driver"))
    if not hal or not Path(hal[1]).resolve().is_relative_to((driver / "lib64").resolve()):
        # common_env 可以建立 driver-shim；其 driver 链接仍必须解析到真实 driver。
        if not hal or not Path(hal[1]).resolve().is_relative_to((driver / "lib64/driver").resolve()):
            raise ValueError("libascend_hal.so 未解析到配置的真实 driver")


def artifact_files(install):
    return {str(path.relative_to(install)): digest(path)
            for path in sorted(install.rglob("*")) if path.is_file() and path.name != "manifest.json"}


def verify_build(config, *, check_run_mode=True):
    manifest = read_json(config.install / "manifest.json")
    if manifest["mode"] != config.mode or manifest["checkpoint"] != config.checkpoint:
        raise ValueError("测试配置与构建不一致，请重新执行 build_all.sh 和 sync.sh")
    if config.checkpoint in (COMBINE_EMPTY_KERNEL_CHECKPOINT, *COMBINE_DEVICE_CHECKPOINTS):
        if manifest.get("server_dedup_combine_contract") != combine_contract(config.checkpoint):
            raise ValueError("Combine 88B launch / 16B GM tiling构建契约或模块身份不符，请重新执行 run_commit_validation.sh")
    if config.checkpoint in (DISPATCH_CHECKPOINT, *COMBINE_DEVICE_CHECKPOINTS):
        if manifest.get("server_dedup_contract") != DISPATCH_ABI:
            raise ValueError("需要完整 Dispatch 120B ABI / 独立 GM tiling 产物，请重新 build_all.sh 和 sync.sh")
        profiling = manifest.get("server_dedup_profiling")
        if config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT and profiling is not False:
            raise ValueError("C7验收不接受profiling ON产物")
        if type(profiling) is not bool or (check_run_mode and config.chain is not None and
                                           profiling is not (config.chain["run_mode"] == "breakdown")):
            raise ValueError("设备打点开关与运行模式不一致：breakdown 请执行 bash build_all.sh breakdown=1，"
                             "普通模式请执行 bash build_all.sh；成功后执行 bash sync.sh")
        if profiling and manifest.get("server_dedup_profile_schema") != measurements.RECORDS_SCHEMA:
            raise ValueError("需要标准 Dispatch records.v1 打点产物，请执行 bash build_all.sh breakdown=1 和 bash sync.sh")
    if manifest["architecture"] != platform.machine():
        raise ValueError("节点 CPU 架构与构建不一致，不能直接分发二进制")
    artifacts = artifact_files(config.install)
    if config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT and any(
            name not in manifest["artifacts"] for name in (COMBINE_BOUNDARY_LIBRARY, COMBINE_BOUNDARY_KERNEL)):
        raise ValueError("C7安装包缺少独立边界探针产物")
    # Incremental deployments retain retired files; only this manifest's files
    # belong to the active package. Local build validation remains exact.
    if config.action in ("verify", "node", "rank"):
        artifacts = {name: artifacts.get(name) for name in manifest["artifacts"]}
    if artifacts != manifest["artifacts"]:
        raise ValueError("安装产物缺失或摘要不一致，请重新构建/同步")
    return manifest


def validation_build_directory(config):
    name = validation_directory(config.checkpoint)
    if config.checkpoint in COMBINE_MEASUREMENT_CHECKPOINTS:
        name += "-profile-on" if config.profiling else "-profile-off"
    return config.root / "build" / name


def source_content_sha256(root, source):
    # inventory excludes deployment settings, generated metadata and build/results.
    files = {name: digest(Path(root) / name) for name in source["files"]}
    return hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def clean_build_directory(config):
    work = validation_build_directory(config)
    # 只删除用户授权的本工具构建目录；先检查路径，不能跟随 build symlink。
    for path in (work.parent, work, config.install.parent, config.install):
        if path.is_symlink():
            raise ValueError(f"构建/安装目录不得为符号链接：{path}")
    work.resolve().relative_to(config.root)
    if work.exists() and not work.is_dir():
        raise ValueError(f"构建路径不是目录：{work}")
    if config.checkpoint not in COMBINE_TESTS:
        (config.logs / "sync.json").unlink(missing_ok=True)
    # 即使本次编译失败，也不能把上一轮安装包当作本次构建使用。
    (config.install / "manifest.json").unlink(missing_ok=True)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    return work


def build(config):
    # Combine preparation has its own 900-second budget; execution starts a
    # separate 180-second budget after the fresh package is ready.
    build_deadline = time.monotonic() + 900 if config.checkpoint in COMBINE_TESTS else None
    config.build_deadline = build_deadline

    def command(*args, **kwargs):
        if build_deadline is not None:
            remaining = build_deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("BUILD_FAILED", "Combine 构建超过 900 秒总期限", timeout=True)
            kwargs["timeout"] = min(kwargs.get("timeout", 180), remaining)
        return execute(*args, **kwargs, start_failure="BUILD_FAILED", exit_failure="BUILD_FAILED")

    build_id = uuid.uuid4().hex
    config.build_id = build_id
    work = clean_build_directory(config)
    require_tools(["cmake", "ctest", "g++", "make"])
    source = inventory(config.root, exclude=(config.hostfile.relative_to(config.root).as_posix(),))
    if config.checkpoint in (*COMBINE_MEASUREMENT_CHECKPOINTS, COMBINE_BOUNDARY_CHECKPOINT):
        source["content_sha256"] = source_content_sha256(config.root, source)
    stage = work / "install"
    log = config.logs / ("build-" + build_id)
    print(f"开始构建 {config.checkpoint}，mode={config.mode}，日志：{log}", flush=True)
    for tool in ("cmake", "g++"):
        command([tool, "--version"], log / (tool + "-version.log"))
    combine_tests = COMBINE_TESTS.get(config.checkpoint)
    combine_c2 = config.checkpoint == COMBINE_EMPTY_KERNEL_CHECKPOINT
    if combine_tests and (config.root / ".git").exists():
        status = command(["git", "status", "--porcelain", "--untracked-files=normal"],
                         log / "source-status.log", cwd=config.root)
        source["working_tree_dirty"] = bool(status.strip())
        source["working_tree_status"] = status.splitlines()
    common = ["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_INSTALL_LIBDIR=lib", f"-DCMAKE_INSTALL_PREFIX={stage}"]
    if config.mode == "production":
        command(["cmake", "-S", config.root, "-B", work / "production", *common,
                 "-DTILEXR_BUILD_EP=ON", "-DBUILD_TESTING=OFF", "-DTILEXR_BUILD_TESTS=OFF",
                 f"-DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING={'ON' if config.profiling else 'OFF'}",
                 f"-DTILEXR_EP_BUILD_MEMORY_BOUNDARY_TEST={'ON' if config.checkpoint == MEMORY_BOUNDARY_CHECKPOINT else 'OFF'}",
                 f"-DTILEXR_EP_BUILD_COMBINE_MEMORY_BOUNDARY_TEST={'ON' if config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT else 'OFF'}"],
                log / "production-configure.log", timeout=600)
        command(["cmake", "--build", work / "production", "-j", config.jobs,
                 *(["--target", "tilexr-ep"] if combine_c2 or config.checkpoint in COMBINE_DEVICE_CHECKPOINTS else []),
                 *(["tilexr-ep-combine-memory-boundary-test"] if config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT else [])],
                log / "production-build.log", timeout=3600)
        command(["cmake", "--install", work / "production"], log / "production-install.log", timeout=180)
        kernels = ["dispatch", "combine", "dispatch_memory", "combine_memory", "dispatch_memory_server_dedup"]
        if combine_c2 or config.checkpoint in COMBINE_DEVICE_CHECKPOINTS:
            kernels.append("combine_memory_server_dedup")
        for kernel in kernels:
            if not (stage / f"lib/tilexr/kernels/tilexr_ep_{kernel}_kernel.o").is_file():
                raise ValueError(f"缺少 EP kernel：{kernel}")
    host_build = work / "host"
    registration = config.checkpoint == EMPTY_KERNEL_CHECKPOINT
    chain = config.checkpoint in CHAIN_CHECKPOINTS
    command(["cmake", "-S", config.root / "tests/ep", "-B", host_build, *common,
             "-DBUILD_TILEXR_EP_DEMO=OFF", "-DBUILD_TILEXR_EP_PLAN_MULTIRANK_TEST=OFF",
             f"-DBUILD_TILEXR_EP_SERVER_DEDUP_COMBINE_REGISTRATION_TEST={'ON' if combine_c2 else 'OFF'}",
             f"-DBUILD_TILEXR_EP_SERVER_DEDUP_REGISTRATION_TEST={'ON' if registration else 'OFF'}",
             f"-DBUILD_TILEXR_EP_SERVER_DEDUP_CHAIN_TEST={'ON' if chain else 'OFF'}",
             f"-DBUILD_TILEXR_EP_MEMORY_BOUNDARY_TEST={'ON' if config.checkpoint == MEMORY_BOUNDARY_CHECKPOINT else 'OFF'}",
             f"-DBUILD_TILEXR_EP_COMBINE_MEMORY_BOUNDARY_TEST={'ON' if config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT else 'OFF'}",
             f"-DTILEXR_INSTALL_PREFIX={stage}",
             f"-DPython3_EXECUTABLE={sys.executable}"], log / "host-configure.log", timeout=600)
    build_targets = combine_tests
    if config.checkpoint in (COMBINE_DRY_RUN_CHECKPOINT, *COMBINE_DEVICE_CHECKPOINTS):
        build_targets = (combine_tests - {COMBINE_DRY_RUN_TEST, COMBINE_CHAIN_TEST, COMBINE_MULTI_TEST, COMBINE_PROFILE_TEST, COMBINE_BOUNDARY_TEST, COMBINE_M12_CONSUMER_TEST, COMBINE_M3_CONSUMER_TEST, COMBINE_CONFIG_TEST}) | {COMBINE_DRY_RUN_PROGRAM}
    if config.checkpoint in COMBINE_DEVICE_CHECKPOINTS:
        build_targets = build_targets | {CHAIN_PROGRAM}
    targets = ["--target", *sorted(build_targets)] if build_targets else []
    command(["cmake", "--build", host_build, "-j", config.jobs, *targets], log / "host-build.log", timeout=1800)
    if not combine_tests:
        command(["cmake", "--install", host_build], log / "host-install.log")
    if config.checkpoint in COMBINE_DEVICE_CHECKPOINTS:
        (stage / "bin").mkdir(parents=True, exist_ok=True)
        shutil.copy2(host_build / CHAIN_PROGRAM, stage / "bin" / CHAIN_PROGRAM)
    if chain and not (stage / "bin" / CHAIN_PROGRAM).is_file():
        raise ValueError(f"Server Dedup 测试未安装：{CHAIN_PROGRAM}")
    try:
        selection = ["-R", "^(" + "|".join(sorted(combine_tests)) + ")$"] if combine_tests else []
        listing = json.loads(command(["ctest", "--test-dir", host_build, "--show-only=json-v1", *selection],
                                     log / "tests.json"))
    except json.JSONDecodeError as error:
        raise Failure("LOG_PARSE_FAILED", "无法解析 CTest 测试清单", log=str(log / "tests.json")) from error
    if (not isinstance(listing, dict) or not isinstance(listing.get("tests"), list)
            or any(not isinstance(test, dict) or not isinstance(test.get("name"), str)
                   or not isinstance(test.get("command"), list) or not test["command"]
                   or any(not isinstance(arg, str) for arg in test["command"]) for test in listing["tests"])):
        raise Failure("LOG_PARSE_FAILED", "CTest 测试清单格式错误", log=str(log / "tests.json"))
    tests = []
    for test in listing["tests"]:
        command = test["command"]
        if test["name"] in (COMBINE_DRY_RUN_TEST, COMBINE_CHAIN_TEST, COMBINE_MULTI_TEST, COMBINE_PROFILE_TEST, COMBINE_BOUNDARY_TEST, COMBINE_M12_CONSUMER_TEST, COMBINE_M3_CONSUMER_TEST):
            if (len(command) != 4 or not command[1].endswith(".py") or command[2] != "--consumer"
                    or Path(command[3]).name != COMBINE_DRY_RUN_PROGRAM):
                raise ValueError(f"无法导出 C3 consumer 测试命令：{command}")
            (stage / "bin").mkdir(parents=True, exist_ok=True)
            shutil.copy2(command[3], stage / "bin" / COMBINE_DRY_RUN_PROGRAM)
            argv = ["@python", str(Path(command[1]).resolve().relative_to(config.root)), "--consumer",
                    str(config.install.relative_to(config.root) / "bin" / COMBINE_DRY_RUN_PROGRAM)]
        elif len(command) == 2 and command[1].endswith(".py"):
            argv = ["@python", str(Path(command[1]).resolve().relative_to(config.root))]
        elif len(command) == 1:
            name = Path(command[0]).name
            if combine_tests:
                # Combine installs only its built targets; the general CMake install
                # also expects unrelated Dispatch executables to be built.
                (stage / "bin").mkdir(parents=True, exist_ok=True)
                shutil.copy2(command[0], stage / "bin" / name)
            if not (stage / "bin" / name).is_file():
                raise ValueError(f"Host 测试未安装：{name}")
            argv = [str(config.install.relative_to(config.root) / "bin" / name)]
        else:
            raise ValueError(f"无法导出测试命令：{command}")
        tests.append({"name": test["name"], "argv": argv})
    names = [test["name"] for test in tests]
    required = combine_tests if combine_tests else REQUIRED_TESTS | ({REGISTRATION_TEST} if registration else set())
    missing = required - set(names)
    if missing or len(names) != len(set(names)) or (combine_tests and set(names) != required):
        raise ValueError(f"必需测试缺失或重复：{sorted(missing)}")
    if config.checkpoint == MEMORY_BOUNDARY_CHECKPOINT:
        # This delivery accepts only the new capacity/access case; no old checkpoint regression.
        tests = []
        for artifact in ("lib/libtilexr-ep-memory-boundary-test.so",
                         "lib/tilexr/kernels/tilexr_ep_memory_boundary_test_kernel.o"):
            if not (stage / artifact).is_file():
                raise ValueError("缺少通信域边界测试产物：" + artifact)
    if config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT:
        for artifact in (COMBINE_BOUNDARY_LIBRARY, COMBINE_BOUNDARY_KERNEL):
            if not (stage / artifact).is_file():
                raise ValueError("C7缺少独立边界探针产物：" + artifact)
    runtime_check(config, stage, log)
    if config.checkpoint in (*COMBINE_MEASUREMENT_CHECKPOINTS, COMBINE_BOUNDARY_CHECKPOINT):
        final_source = inventory(config.root, exclude=(config.hostfile.relative_to(config.root).as_posix(),))
        if (final_source["commit"] != source["commit"] or
                source_content_sha256(config.root, final_source) != source["content_sha256"]):
            raise Failure("BUILD_FAILED", "构建期间源码发生变化，拒绝关联不确定产物")
    manifest = {"build_id": build_id, "source": source, "mode": config.mode,
                "checkpoint": config.checkpoint, "architecture": platform.machine(),
                "python": sys.version, "cann": os.environ.get("ASCEND_HOME_PATH", ""),
                "driver": os.environ.get("ASCEND_DRIVER_PATH", ""), "tests": tests,
                "artifacts": artifact_files(stage)}
    if combine_tests:
        manifest["build_timeout_seconds"] = 900
    if combine_c2 or config.checkpoint in COMBINE_DEVICE_CHECKPOINTS:
        manifest["server_dedup_combine_contract"] = dict(combine_contract(config.checkpoint))
    if config.checkpoint in COMBINE_MEASUREMENT_CHECKPOINTS:
        for operator, filename in (("dispatch", "server_dedup.mapping.json"), ("combine", "server_dedup_combine.mapping.json")):
            if not (stage / "share/tilexr/profiling" / filename).is_file():
                raise ValueError("C6缺少独立profiling mapping：" + operator)
    if config.checkpoint in (DISPATCH_CHECKPOINT, *COMBINE_DEVICE_CHECKPOINTS):
        manifest["server_dedup_contract"] = dict(DISPATCH_ABI)
        manifest["server_dedup_profiling"] = config.profiling
        if manifest["server_dedup_profiling"]:
            manifest["server_dedup_profile_schema"] = measurements.RECORDS_SCHEMA
    if build_deadline is not None and time.monotonic() >= build_deadline:
        raise Failure("BUILD_FAILED", "Combine 构建超过 900 秒总期限", timeout=True)
    write_json(stage / "manifest.json", manifest)
    config.install.parent.mkdir(parents=True, exist_ok=True)
    if config.install.is_symlink() or config.install.parent.is_symlink():
        raise ValueError("install/validation 不得为符号链接")
    if config.install.exists():
        shutil.rmtree(config.install)  # 仅重建本工具拥有的安装目录。
    stage.replace(config.install)
    if not combine_tests or config.checkpoint in COMBINE_DEVICE_CHECKPOINTS:
        write_json(config.root / ".tilexr-source.json", source)
        (config.root / ".tilexr-validation-root").write_text("TileXR validation deployment\n", encoding="utf-8")
    print(f"BUILD_PASS commit={source['commit'] or 'unknown'} "
          f"mode={config.mode} tests={len(tests)}\n日志：{log}", flush=True)


def remote_argv(config, host, action, *arguments):
    command = "bash -s -- " + " ".join(shlex.quote(str(value)) for value in (config.root, action, *arguments))
    if action in ("verify", "node", "cancel"):
        values = {
            "TILEXR_VALIDATION_CHECKPOINT": config.checkpoint,
            "TILEXR_VALIDATION_BUILD_MODE": config.mode,
            "TILEXR_VALIDATION_RESULT_ROOT": str(config.logs),
            "TILEXR_VALIDATION_BREAKDOWN": int(config.install.name.endswith("-profile-on")) if config.checkpoint in COMBINE_MODULE_CHECKPOINTS else int(config.profiling),
        }
        if action == "node":
            values.update(TILEXR_VALIDATION_SERVERS=config.server_count,
                          TILEXR_DIRECT_HOSTS_JSON=json.dumps(config.hosts(), separators=(",", ":")))
            if config.chain:
                for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "quant_mode", "operator_mode", "dispatch_quant_mode", "combine_quant_mode", "fp8_format",
                            "block_dim", "loops", "warmup", "run_mode", "msprof", "case_analysis", "route_mode", "seed", "combine_fixture", "combine_observation"):
                    values["TILEXR_VALIDATION_" + key.upper()] = config.chain.get(key)
                values["TILEXR_VALIDATION_BOUNDARY_PROBE"] = int(config.chain.get("boundary_probe", False))
                values["TILEXR_COMM_ID"] = config.chain["comm_id"]
                values["TILEXR_DEMO_BARRIER_ADDR"] = config.chain["barrier_addr"]
        exports = "; ".join("export " + key + "=" + shlex.quote(str(value))
                             for key, value in values.items() if value is not None)
        command = "TILEXR_DIRECT_CONFIG=" + shlex.quote(exports) + " " + command
    return ["ssh", *SSH_OPTIONS, config.target(host), command]


def sync_paths(config, source, manifest):
    """Runtime closure plus the installed Python tests' source dependencies."""
    prefixes = ["tools/validation/", "tools/profiling/", "tools/ep/"]
    if manifest and any(test["argv"][0] == "@python" for test in node_tests(manifest)):
        # The address-space test compiles the production kernel with fake APIs.
        prefixes.extend(["tests/ep/python/", "tests/ep/unit/server_dedup_count_fake/",
                         "src/include/", "src/ep/common/", "src/ep/kernels/"])
    names = {"set_env.sh", "scripts/common_env.sh", "scripts/common_util.sh",
             "src/ep/profiling/server_dedup.mapping.json"}
    if manifest:
        names.update(arg for test in node_tests(manifest) for arg in test["argv"] if arg != "@python")
    paths = {name for name in source["files"]
             if name in names or name.startswith(tuple(prefixes))}
    paths.discard(config.hostfile.relative_to(config.root).as_posix())
    for name in (".tilexr-source.json", ".tilexr-validation-root",
                 config.install.relative_to(config.root).as_posix() + "/", "validation.local.sh"):
        if (config.root / name).exists():
            paths.add(name)
    return paths


def sync_debug(config):
    """Probe every configured endpoint without preparing or changing deployment files."""
    hosts = config.hosts()
    require_tools(["ssh", "rsync"])
    log = config.logs / ("sync-debug-" + uuid.uuid4().hex)
    source = inventory(config.root, exclude=(config.hostfile.relative_to(config.root).as_posix(),))
    manifest_path = config.install / "manifest.json"
    paths = sync_paths(config, source, read_json(manifest_path) if manifest_path.exists() else None)
    file_list = log / "files.txt"
    file_list.parent.mkdir(parents=True, exist_ok=True)
    file_list.write_text("\n".join(sorted(paths)) + "\n", encoding="utf-8")
    ssh = ["ssh", "-vvv", *SSH_OPTIONS]
    nodes = []
    for index, host in enumerate(hosts):
        target = config.target(host["host"])
        node_log = log / str(index)
        result = dict(index=index, host=host["host"], target=target, status="PASS", checks=[])
        checks = [
            ("ssh_probe", [*ssh, target, "hostname && command -v rsync && rsync --version"], 30),
            ("rsync_dry_run", ["rsync", "-a", "-r", "--dry-run", "--verbose", "--itemize-changes",
                               "--checksum", "--protect-args", "--timeout=30",
                               "--files-from=" + str(file_list), "-e", shlex.join(ssh),
                               str(config.root) + "/", target + ":" + str(config.root) + "/"], 180),
        ]
        for stage, argv, timeout in checks:
            check = dict(stage=stage, log=str(node_log / (stage + ".log")), status="PASS")
            try:
                execute(argv, check["log"], timeout=timeout,
                        start_failure="SYNC_FAILED", exit_failure="SYNC_FAILED")
            except Exception as error:
                check.update(status="FAIL", error=failure_record(error, "SYNC_FAILED"))
                result["status"] = "FAIL"
            result["checks"].append(check)
            print(f"SYNC_DEBUG_{check['status']} host={target} stage={stage} log={check['log']}", flush=True)
        nodes.append(result)
    failed = [node["target"] for node in nodes if node["status"] == "FAIL"]
    write_json(log / "summary.json", dict(status="FAIL" if failed else "PASS", dry_run=True,
                                         nodes=nodes, failed_hosts=failed))
    print(f"SYNC_DEBUG_SUMMARY checked={len(nodes)} failed={len(failed)} "
          f"failed_hosts={','.join(failed) or '-'}\n结果：{log / 'summary.json'}\n"
          "dry-run 不写入远端部署文件；通过不代表实际数据传输或并发同步稳定。", flush=True)
    return FAILURE_CODES["SYNC_FAILED"] if failed else 0


def sync(config):
    def command(*args, **kwargs):
        return execute(*args, **kwargs, start_failure="SYNC_FAILED", exit_failure="SYNC_FAILED")

    hosts = config.hosts()
    check_coordinator(config, hosts)
    if len(hosts) > 1:
        require_tools(["ssh", "rsync"])
    log = config.logs / ("sync-" + uuid.uuid4().hex)
    # Distribution validates the artifact itself; run/node enforce the selected case mode.
    manifest = verify_build(config, check_run_mode=False)
    receipt = config.logs / "sync.json"
    receipt.unlink(missing_ok=True)
    source = inventory(config.root, exclude=(config.hostfile.relative_to(config.root).as_posix(),))
    write_json(config.root / ".tilexr-source.json", source)
    paths = sync_paths(config, source, manifest)
    file_list = log / "files.txt"
    file_list.parent.mkdir(parents=True, exist_ok=True)
    file_list.write_text("\n".join(sorted(paths)) + "\n", encoding="utf-8")

    def send(index, host):
        node_log = log / str(index)
        prepare = '''set -eo pipefail
python3 - "$1" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
if not root.is_absolute() or root == pathlib.Path('/') or '..' in root.parts:
    raise SystemExit('invalid deployment root')
if any(path.is_symlink() for path in (root, *root.parents)):
    raise SystemExit('deployment root traverses a symlink')
marker = root / '.tilexr-validation-root'
if (root / '.git').exists():
    raise SystemExit('refusing to overwrite a Git checkout; use a deployment directory')
if root.exists() and any(root.iterdir()) and (
    not marker.is_file() or marker.is_symlink() or marker.read_text() != 'TileXR validation deployment\\n'):
    raise SystemExit('destination is not a managed validation directory')
for lock in ('.operation.lock', 'node.lock'):
    if (root / 'run/validation' / lock).exists():
        raise SystemExit('destination has an active or unconfirmed validation task')
install = root / 'install'
if install.is_symlink() or any(path.is_symlink() for path in install.glob('validation*')):
    raise SystemExit('installation directory must not be a symlink')
root.mkdir(parents=True, exist_ok=True)
PY
'''
        command(["ssh", *SSH_OPTIONS, config.target(host["host"]),
                 "bash -s -- " + shlex.quote(str(config.root))],
                node_log / "prepare.log", stdin=prepare)
        command(["rsync", "-a", "-r", "--checksum", "--protect-args", "--timeout=30",
                 "--files-from=" + str(file_list), "-e", shlex.join(["ssh", *SSH_OPTIONS]),
                 str(config.root) + "/", config.target(host["host"]) + ":" + str(config.root) + "/"],
                node_log / "transfer.log", timeout=180)
        command(remote_argv(config, host["host"], "verify", manifest["build_id"]),
                node_log / "verify.log", timeout=60, stdin=REMOTE_SCRIPT)
        return host["host"]

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        futures = [pool.submit(send, index, host) for index, host in enumerate(hosts) if index]
        for future in concurrent.futures.as_completed(futures):
            print(f"SYNC_NODE_PASS host={future.result()}", flush=True)
    write_json(receipt, {"build_id": manifest["build_id"], "artifacts": manifest["artifacts"],
                        "install": str(config.install.relative_to(config.root)),
                        "hosts": [host["host"] for host in hosts]})
    print(f"SYNC_PASS hosts={len(hosts)} build_id={manifest['build_id']}\n日志：{log}", flush=True)


def pid_identity(pid):
    try:
        # Linux /proc starttime 防止取消旧运行时命中复用的 PID。
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]
    except FileNotFoundError:
        return None


def rank_mapping(hosts):
    if not 2 <= len(hosts) <= 16 or any(len(host["devices"]) != 8 for host in hosts):
        raise ValueError("多 rank 检查点要求 2 至 16 台主机，每台恰好 8 个 device ID；完整验收分别使用 64P 和 128P")
    return [{"host": host["host"], "host_index": index, "local_rank": local_rank,
             "device": device, "rank": index * 8 + local_rank, "rank_size": len(hosts) * 8}
            for index, host in enumerate(hosts) for local_rank, device in enumerate(host["devices"])]


def rank_record(mapping, run_id, manifest):
    return dict(mapping, run_id=run_id, build_id=manifest["build_id"], commit=manifest["source"]["commit"],
                checkpoint=manifest["checkpoint"], status="PASS")


def dry_run_rank(config, run_id, build_id, rank):
    # 每 rank 一个普通进程，只验证分配的身份；不加载 ACL、不设置设备、不创建 communicator。
    if config.checkpoint != DRY_RUN_CHECKPOINT:
        raise ValueError("rank 入口只用于 dry-run")
    mappings = rank_mapping(config.hosts())
    rank = int(rank)
    if not 0 <= rank < len(mappings):
        raise ValueError("dry-run rank 超出 hostfile 范围")
    # 安装产物已由节点核对一次；逐 rank 只读取调用身份，不重复摘要/运行库预检。
    manifest = read_json(config.install / "manifest.json")
    if (manifest["build_id"] != build_id or manifest["checkpoint"] != config.checkpoint
            or manifest["mode"] != config.mode):
        raise Failure("RESULT_FAILED", "rank 的构建或配置不属于本次运行")
    record = rank_record(mappings[rank], identifier(run_id), manifest)
    record["pid"] = os.getpid()
    print(DRY_RUN_MARKER + json.dumps(record, ensure_ascii=False), flush=True)
    return 0


def validate_dispatch_tables(records, config):
    """All ranks must match the same global route oracle locally; no table exchange."""
    ranks = [record.get("rank") for record in records]
    if (len(ranks) != config["rank_size"] or any(type(rank) is not int for rank in ranks)
            or set(ranks) != set(range(config["rank_size"]))):
        raise ValueError("五表校验 Rank 缺失、重复或越界")
    identity = ("run_id", "build_id", "commit", "bs", "h", "topk", "moe_expert_num", "dtype",
                "quant_mode", "fp8_format", "route_mode", "seed", "server_dedup_enabled")
    for record in records:
        if any(key not in record or record[key] != records[0].get(key) for key in identity):
            raise ValueError("五表校验不属于同一构建和 case")
        if any(key in config and record[key] != config[key] for key in identity):
            raise ValueError("五表校验与本次 case 不一致")
        if any(record.get(key) is not True for key in
               ("dispatch_outputs_validated", "five_tables_validated", "member_addresses_validated")):
            raise ValueError("缺少各 Rank 五表及成员地址本地匹配校验")
    if sum(record[kind + "_packets"] for record in records for kind in ("first_hop", "direct", "relay")) != (
            config["rank_size"] * config["bs"] * config["topk"]):
        raise ValueError("完整 Dispatch 物理包总数与原始 route 总数不一致")


def validate_combine_dispatch_record(dispatch, chain, mapping, run_id, manifest, config, *, allow_invalid_output=False):
    """Retain the Combine checkpoint identity while applying the complete Dispatch validator."""
    if dispatch.get("checkpoint") != manifest["checkpoint"] or dispatch.get("pid") != chain["pid"]:
        raise Failure("RESULT_FAILED", "Dispatch 不属于本次进程/检查点", rank=mapping["rank"])
    if manifest["checkpoint"] == COMBINE_BOUNDARY_CHECKPOINT and dispatch.get("boundary_probe") is not False:
        raise Failure("RESULT_FAILED", "C7真实Dispatch不得使用边界探针标记", rank=mapping["rank"])
    adapted = dict(dispatch, checkpoint=DISPATCH_CHECKPOINT,
                   chain=dict(chain, checkpoint=DISPATCH_CHECKPOINT))
    validate_rank_records([adapted], [mapping], run_id, dict(manifest, checkpoint=DISPATCH_CHECKPOINT),
                          dict(config, kernel_scope="dispatch"), check_measurements=False, allow_invalid_output=allow_invalid_output)


def validate_multi_magic(record, config, *, dispatch_only=False):
    """Validate caller-derived magic without consuming another communicator round."""
    total = config["loops"] + config["warmup"]
    both = config["operator_mode"] == "both"
    numbers = {"stream_synchronizations": total if config.get("combine_observation") == "per-round" else 1}
    for operator in ("dispatch", "combine"):
        if dispatch_only and operator != "dispatch":
            continue
        active = config["operator_mode"] in (operator, "both")
        first = (2 if operator == "combine" and both else 1) if active else 0
        last = (2 * total - int(operator == "dispatch") if both else total) if active else 0
        mask = (1 << (first % 2) if both or total == 1 else 3) if active else 0
        numbers[operator + "_first_magic"] = first
        numbers[operator + "_window_mask"] = mask
        numbers["last_magic" if dispatch_only else operator + "_last_magic"] = last
        numbers["window_offset" if dispatch_only else operator + "_window_offset"] = (last % 2) * MEMORY_BOUNDARY_BYTES
        if not dispatch_only:
            for kind, count in (("", config["loops"]), ("warmup_", config["warmup"])):
                for state in ("submitted", "completed"):
                    numbers[operator + "_" + kind + state] = count if active else 0
    if not dispatch_only:
        numbers.update(warmup_submitted=config["warmup"], warmup_completed=config["warmup"], combine_delay_us=0 if config.get("implemented_through") == "M3" else 50)
    for key, value in numbers.items():
        if type(record.get(key)) is not int or record[key] != value:
            raise Failure("RESULT_FAILED", "C5 提交/完成、预热、magic/选窗或同步记录不一致", field=key)


def validate_combine_multi_record(record, mapping, run_id, manifest, config, *, invalid_output=False):
    both = config["operator_mode"] == "both"
    if config["operator_mode"] == "dispatch":
        validate_combine_dispatch_record(record, record["chain"], mapping, run_id, manifest, config, allow_invalid_output=invalid_output)
        validate_multi_magic(record, config, dispatch_only=True)
        return
    validate_multi_magic(record, config)
    if invalid_output:
        if both:
            dispatch = record.get("dispatch")
            if not isinstance(dispatch, dict):
                raise Failure("LOG_PARSE_FAILED", "M3 invalid result lacks completed Dispatch identity", rank=mapping["rank"])
            validate_combine_dispatch_record(dispatch, record["chain"], mapping, run_id, manifest, config,
                                            allow_invalid_output=True)
            validate_multi_magic(dispatch, config, dispatch_only=True)
        return
    independent = not both or config["dispatch_quant_mode"] == 4
    flags = dict(combine_empty=config.get("implemented_through") not in ("M2", "M3"), combine_inputs_unchanged=not both, y_out_unchanged=True,
                 independent_expert_out_unchanged=independent, five_tables_linked=both, send_counts_linked=both)
    source = ("separate_expert_out" if independent else "dispatch_expand_x") if both else "standalone_expert_out"
    if (any(record.get(key) is not value for key, value in flags.items()) or
            record.get("kernel_scope") != config["kernel_scope"] or
            record.get("expert_out_source") != source or
            record.get("combine_input_check") != ("dispatch-final-reference" if both else "full-snapshot")):
        raise Failure("RESULT_FAILED", "C5 输入检查范围、生命周期或原指针关联记录不一致", rank=mapping["rank"])
    if both:
        dispatch = record.get("dispatch")
        if not isinstance(dispatch, dict):
            raise Failure("LOG_PARSE_FAILED", "联合模式缺少真实 Dispatch PASS", rank=mapping["rank"])
        validate_combine_dispatch_record(dispatch, record["chain"], mapping, run_id, manifest, config, allow_invalid_output=invalid_output)
        validate_multi_magic(dispatch, config, dispatch_only=True)
    elif "dispatch" in record:
        raise Failure("RESULT_FAILED", "Combine 独跑意外提交 Dispatch", rank=mapping["rank"])


def validate_combine_m12_record(record, mapping, config):
    for marker in (record, record["chain"]):
        for key in ("implemented_through", "scope", "combine_fixture", "combine_observation"):
            if marker.get(key) != config[key]:
                raise Failure("RESULT_FAILED", "P2.1模块或诊断配置不一致", field=key, rank=mapping["rank"])
    if config["operator_mode"] == "dispatch":
        return
    required = ("combine_stage_validated", "combine_control_validated", "combine_control_reserved_unchanged", "combine_result_unchanged")
    if (record.get("native_status") != "combine-m12" or
            any(record.get(key) is not True for key in required)):
        raise Failure("RESULT_FAILED", "P2.1缺少stage/control/保护区真实校验证据", rank=mapping["rank"])
    rows = record.get("combine_type3_rows")
    if type(rows) is not int or not 0 <= rows <= 10000:
        raise Failure("RESULT_FAILED", "P2.1 type3行数非法", rank=mapping["rank"])
    token_num = record.get("combine_token_num")
    if type(token_num) is not int or not rows <= token_num <= 10000:
        raise Failure("RESULT_FAILED", "P2.1 TokenNum非法", rank=mapping["rank"])
    if config["combine_fixture"] != "balanced":
        expected_count = (0 if config["combine_fixture"] == "empty" else min(10000,
            config["bs"] * mapping["rank_size"] * min(config["topk"], config["moe_expert_num"] // mapping["rank_size"])))
        expected_rows = sum(row % 7 == 2 or row % 11 == 5 for row in range(expected_count))
        if token_num != expected_count or rows != expected_rows:
            raise Failure("RESULT_FAILED", "P2.1定向fixture未覆盖预定TokenNum/type3行数", rank=mapping["rank"])
    numbers = dict(combine_algorithm_bytes=rows * config["h"] * 2,
                   combine_stage_copy_bytes=rows * config["h"] * 4,
                   combine_control_publish_bytes=(64 * 8 + 8 * (64 - config["combine_send_core_num"])) * 32,
                   combine_observed_rounds=(config["loops"] + config["warmup"]
                                            if config["combine_observation"] == "per-round" else 1),
                   combine_checked_window_mask=record.get("combine_window_mask"))
    for key, value in numbers.items():
        if type(record.get(key)) is not int or record[key] != value:
            raise Failure("RESULT_FAILED", "P2.1字节/轮次/选窗校验不一致", field=key, rank=mapping["rank"])


def validate_combine_m3_record(record, mapping, config, *, invalid_output=False):
    for marker in (record, record["chain"]):
        for key in ("implemented_through", "scope", "combine_fixture", "combine_observation"):
            if marker.get(key) != config[key]:
                raise Failure("RESULT_FAILED", "M3 module/configuration identity mismatch", field=key, rank=mapping["rank"])
    if config["loops"] != 1 or config["warmup"] != 0:
        raise Failure("RESULT_FAILED", "M3 only qualifies one launch without warmup")
    if config["operator_mode"] == "dispatch":
        return
    if record.get("native_status") != "combine-m3":
        raise Failure("RESULT_FAILED", "M3 native implementation identity mismatch", rank=mapping["rank"])
    if invalid_output:
        if (record.get("execution_completed") is not True or
                not isinstance(record.get("validation_error"), str) or not record["validation_error"] or
                not isinstance(record.get("validation_stage"), str) or not record["validation_stage"]):
            raise Failure("LOG_PARSE_FAILED", "M3 invalid output lacks execution completion/error evidence", rank=mapping["rank"])
    else:
        flags = ("combine_stage_validated", "combine_control_validated", "combine_control_reserved_unchanged",
                 "combine_result_validated", "combine_result_flags_validated", "combine_unwritten_results_unchanged")
        if any(record.get(key) is not True for key in flags):
            raise Failure("RESULT_FAILED", "M3 stage/control/DIRECT payload/Flag/protection validation missing", rank=mapping["rank"])
    values = [record.get(key) for key in ("combine_type3_rows", "combine_type1_rows", "combine_token_num", "combine_direct_received_rows")]
    if (any(type(value) is not int or not 0 <= value <= 10000 for value in values[:3]) or
            type(values[3]) is not int or not 0 <= values[3] <= config["bs"] * config["topk"]):
        raise Failure("RESULT_FAILED", "M3 row counts invalid", rank=mapping["rank"])
    stage_rows, direct_rows, tokens, received = values
    if stage_rows + direct_rows > tokens:
        raise Failure("RESULT_FAILED", "M3 disjoint row counts exceed TokenNum", rank=mapping["rank"])
    payload = direct_rows * config["h"] * 2
    wire = direct_rows * ((config["h"] * 2 + 479) // 480) * 512
    numbers = dict(combine_algorithm_bytes=(stage_rows + direct_rows) * config["h"] * 2,
                   combine_stage_copy_bytes=stage_rows * config["h"] * 4,
                   combine_direct_payload_bytes=payload, combine_direct_wire_bytes=wire,
                   combine_control_publish_bytes=(64 * 8 + 8 * (64 - config["combine_send_core_num"])) * 32,
                   combine_checked_window_mask=record.get("combine_window_mask"))
    observed = record.get("combine_observed_rounds")
    if type(observed) is not int or observed not in ((0, 1) if invalid_output else (1,)):
        raise Failure("RESULT_FAILED", "M3 observation count does not match result status", rank=mapping["rank"])
    if any(type(record.get(key)) is not int or record[key] != value for key, value in numbers.items()):
        raise Failure("RESULT_FAILED", "M3 traffic/observation/window evidence mismatch", rank=mapping["rank"])


def validate_combine_boundary_record(record, mapping):
    stride = SERVER_DEDUP_PEER_BYTES // 2
    reserved = 4 * 1024 ** 2
    numbers = dict(peer_mem_bytes=SERVER_DEDUP_PEER_BYTES, buffer_stride=stride,
                   runtime_reserved_bytes=reserved, legal_window_bytes=stride - reserved,
                   probe_bytes=32, probe_submitted=8, probe_completed=8, submitted=8, completed=8,
                   warmup_submitted=0, warmup_completed=0, dispatch_submitted=0, dispatch_completed=0,
                   combine_submitted=0, combine_completed=0)
    if any(type(record.get(key)) is not int or record[key] != value for key, value in numbers.items()):
        raise Failure("RESULT_FAILED", "C7探针容量或提交/完成计数不一致", rank=mapping["rank"])
    if record.get("kernel_scope") != "memory-boundary" or record.get("native_status") != "boundary-probe":
        raise Failure("RESULT_FAILED", "C7边界探针执行范围不明确", rank=mapping["rank"])
    points = record.get("points")
    if not isinstance(points, list) or len(points) != 8 or any(not isinstance(point, dict) for point in points):
        raise Failure("LOG_PARSE_FAILED", "C7必须包含双窗全部8个独立点位", rank=mapping["rank"])
    offsets = (764 * 1024, 860 * 1024 - 32, 1024 ** 2, stride - reserved - 32)
    for number, point in enumerate(points):
        window, index = divmod(number, 4)
        expected = dict(window=window, index=index, offset=window * stride + offsets[index],
                        pattern=0xC7000101 + window * 0x100 + index * 0x10,
                        initial_zero=True, device_initial_zero=True, device_readback=True, host_readback=True,
                        guard_before_zero=True, guard_after_checked=index != 3,
                        guard_after_zero=True if index != 3 else None, final_readback=True)
        if any(key not in point or type(point[key]) is not type(value) or point[key] != value for key, value in expected.items()):
            raise Failure("RESULT_FAILED", "C7点位/窗口/pattern/保护区或最终复核不完整", rank=mapping["rank"], point=number)


def validate_combine_chain_record(record, mapping, run_id, manifest, config, *, invalid_output=False):
    """Certify empty Combine evidence while retaining the complete Dispatch checks."""
    if config is None:
        raise Failure("RESULT_FAILED", "缺少 Combine 配置", rank=mapping["rank"])
    both = config["operator_mode"] == "both"
    if manifest["checkpoint"] == COMBINE_BOUNDARY_CHECKPOINT:
        for marker in (record, record["chain"]):
            if marker.get("boundary_probe") is not config["boundary_probe"]:
                raise Failure("RESULT_FAILED", "C7 boundary_probe回显不一致", rank=mapping["rank"])
    for marker in (record, record["chain"]):
        for key in ("operator_mode", "dispatch_quant_mode", "combine_quant_mode", "quant_mode",
                    "fp8_format", "run_mode", "loops", "warmup"):
            if type(marker.get(key)) is not type(config[key]) or marker[key] != config[key]:
                raise Failure("RESULT_FAILED", "Combine 配置回显不一致", rank=mapping["rank"], field=key)
    if manifest["checkpoint"] == COMBINE_M12_CHECKPOINT:
        validate_combine_m12_record(record, mapping, config)
    if manifest["checkpoint"] == COMBINE_M3_CHECKPOINT:
        validate_combine_m3_record(record, mapping, config, invalid_output=invalid_output)
    if config.get("boundary_probe"):
        validate_combine_boundary_record(record, mapping)
        return
    if manifest["checkpoint"] in COMBINE_CONTINUOUS_CHECKPOINTS:
        validate_combine_multi_record(record, mapping, run_id, manifest, config, invalid_output=invalid_output)
        return
    if config["operator_mode"] == "dispatch":
        validate_combine_dispatch_record(record, record["chain"], mapping, run_id, manifest, config)
        return
    numbers = dict(dispatch_submitted=int(both), dispatch_completed=int(both),
                   combine_submitted=1, combine_completed=1, warmup_submitted=0, warmup_completed=0,
                   dispatch_last_magic=int(both), combine_last_magic=2 if both else 1,
                   dispatch_window_offset=MEMORY_BOUNDARY_BYTES if both else 0,
                   combine_window_offset=0 if both else MEMORY_BOUNDARY_BYTES, combine_delay_us=50)
    if any(type(record.get(key)) is not int or record[key] != value for key, value in numbers.items()):
        raise Failure("RESULT_FAILED", "Combine C4 提交/完成计数、magic、选窗或空算子延迟不一致", rank=mapping["rank"])
    if (record.get("kernel_scope") != config["kernel_scope"] or
            any(record.get(key) is not True for key in ("combine_empty", "combine_inputs_unchanged", "y_out_unchanged")) or
            any(record.get(key) is not both for key in ("five_tables_linked", "send_counts_linked"))):
        raise Failure("RESULT_FAILED", "Combine C4 生命周期或原指针连接证据不完整", rank=mapping["rank"])
    source = ("separate_expert_out" if config["dispatch_quant_mode"] else "dispatch_expand_x") if both else "standalone_expert_out"
    if record.get("expert_out_source") != source:
        raise Failure("RESULT_FAILED", "Combine expertOut 类型或来源不一致", rank=mapping["rank"])
    if both:
        dispatch = record.get("dispatch")
        if not isinstance(dispatch, dict):
            raise Failure("LOG_PARSE_FAILED", "联合模式缺少真实 Dispatch PASS", rank=mapping["rank"])
        validate_combine_dispatch_record(dispatch, record["chain"], mapping, run_id, manifest, config)
    elif "dispatch" in record:
        raise Failure("RESULT_FAILED", "Combine 独跑意外提交 Dispatch", rank=mapping["rank"])


def validate_rank_records(records, expected, run_id, manifest, chain_config=None, *, check_measurements=True,
                          allow_invalid_output=False):
    if not isinstance(records, list) or any(not isinstance(record, dict) for record in records):
        raise Failure("LOG_PARSE_FAILED", "rank 记录格式错误")
    actual = [record.get("rank") for record in records]
    if (len(records) != len(expected) or any(type(rank) is not int for rank in actual)
            or set(actual) != {mapping["rank"] for mapping in expected}):
        raise Failure("RESULT_FAILED", "rank 缺失、重复或不属于本节点")
    for record, mapping in zip(sorted(records, key=lambda item: item["rank"]), expected):
        wanted = rank_record(mapping, run_id, manifest)
        invalid_output = (allow_invalid_output and manifest["checkpoint"] in (DISPATCH_CHECKPOINT, COMBINE_M3_CHECKPOINT)
                          and chain_config is not None
                          and record.get("validation_status") == "FAIL")
        chain = manifest["checkpoint"] in CHAIN_CHECKPOINTS
        for marker in (record, record.get("chain")) if chain else (record,):
            if not isinstance(marker, dict):
                raise Failure("LOG_PARSE_FAILED", "缺少 rank 的建链记录", rank=mapping["rank"])
            expected_marker = dict(wanted, status="FAIL") if marker is record and invalid_output else wanted
            if any(marker.get(key) != value for key, value in expected_marker.items()):
                raise Failure("RESULT_FAILED", "rank 身份或设备映射不一致", rank=mapping["rank"])
            if type(marker.get("pid")) is not int or marker["pid"] <= 0:
                raise Failure("LOG_PARSE_FAILED", "rank 缺少有效的进程 PID", rank=mapping["rank"])
            if chain_config is not None and any(marker.get(key) != chain_config[key]
                    for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "route_mode", "seed")):
                raise Failure("RESULT_FAILED", "rank 的实际 shape/dtype/route_mode/seed 与本次配置不一致", rank=mapping["rank"])
        if chain:
            linked = record["chain"]
            if manifest["checkpoint"] in COMBINE_DEVICE_CHECKPOINTS:
                validate_combine_chain_record(record, mapping, run_id, manifest, chain_config, invalid_output=invalid_output)
                if manifest["checkpoint"] in COMBINE_MEASUREMENT_CHECKPOINTS and check_measurements:
                    try:
                        operator_measure.validate_operator_measurements(record, chain_config, run_id=run_id,
                            kernel_shas=operator_kernel_shas(manifest, chain_config))
                    except (ValueError, TypeError, KeyError) as error:
                        raise Failure("RESULT_FAILED", f"C6算子测量记录无效：{error}", rank=mapping["rank"]) from error
            loops = (8 if chain_config and chain_config.get("boundary_probe") else
                     chain_config["loops"] if manifest["checkpoint"] in COMBINE_CONTINUOUS_CHECKPOINTS else 1)
            if manifest["checkpoint"] == DISPATCH_CHECKPOINT:
                if chain_config is None:
                    raise Failure("RESULT_FAILED", "缺少本次 Dispatch 配置", rank=mapping["rank"])
                for marker in (record, linked):
                    enabled = marker.get("server_dedup_enabled")
                    if (type(enabled) is not bool or enabled is not records[0].get("server_dedup_enabled") or
                            ("server_dedup_enabled" in chain_config and
                             enabled is not chain_config["server_dedup_enabled"])):
                        raise Failure("RESULT_FAILED", "Dispatch 编译期去重状态缺失或 Rank 记录不一致", rank=mapping["rank"])
                for key in ("quant_mode", "fp8_format", "kernel_scope"):
                    if record.get(key) != chain_config[key]:
                        raise Failure("RESULT_FAILED", "Dispatch 实际类型或范围不一致", rank=mapping["rank"], field=key)
                stride = source_send_stride(chain_config)
                if record.get("native_status") != "dispatch" or record.get("token_stride") != stride:
                    raise Failure("RESULT_FAILED", "Dispatch 执行范围或 token stride 不一致", rank=mapping["rank"])
                if invalid_output:
                    if (record.get("validation_stage") not in ("count-read", "dispatch-read") or
                            not isinstance(record.get("validation_error"), str) or not record["validation_error"]):
                        raise Failure("LOG_PARSE_FAILED", "缺少明确的输出校验失败原因", rank=mapping["rank"])
                else:
                    if any(record.get(key) is not True for key in
                           ("first_hop_validated", "direct_validated", "relay_validated", "route_coverage_validated",
                            "source_outputs_validated", "weights_unchanged", "dispatch_outputs_validated",
                            "five_tables_validated", "member_addresses_validated", "flags_cleared")):
                        raise Failure("RESULT_FAILED", "Dispatch 数据、五表、成员地址或轮末清理缺少完整校验", rank=mapping["rank"])
                    try:
                        measurements.validate_dispatch_traffic(record, dict(chain_config,
                            server_dedup_enabled=record["server_dedup_enabled"]))
                    except (ValueError, TypeError, KeyError) as error:
                        raise Failure("RESULT_FAILED", f"Dispatch 流量记录无效：{error}", rank=mapping["rank"]) from error
                loops = chain_config["loops"]
                for marker in (record, linked):
                    if any(marker.get(key) != chain_config[key] for key in ("run_mode", "loops", "warmup")):
                        raise Failure("RESULT_FAILED", "rank 的模式或批次配置不一致", rank=mapping["rank"])
                if any(type(record.get(key)) is not int or record[key] != chain_config["warmup"]
                       for key in ("warmup_submitted", "warmup_completed")):
                    raise Failure("RESULT_FAILED", "rank 未完成指定预热批次", rank=mapping["rank"])
                if check_measurements and chain_config["run_mode"] != "functional":
                    try:
                        if (chain_config["run_mode"] == "breakdown" and
                                record.get("breakdown", {}).get("schema") != manifest.get("server_dedup_profile_schema")):
                            raise ValueError("breakdown 设备记录版本与安装产物不一致")
                        measurements.validate_rank_measurements(record, chain_config)
                    except (ValueError, TypeError, KeyError) as error:
                        raise Failure("RESULT_FAILED", f"rank 测量记录无效：{error}", rank=mapping["rank"]) from error
            expected_peer_bytes = (MEMORY_BOUNDARY_BYTES if manifest["checkpoint"] == MEMORY_BOUNDARY_CHECKPOINT
                                   else SERVER_DEDUP_PEER_BYTES)
            if type(linked.get("peer_mem_bytes")) is not int or linked["peer_mem_bytes"] != expected_peer_bytes:
                raise Failure("RESULT_FAILED", "rank peer window 容量不符合当前检查点",
                              rank=mapping["rank"], expected_bytes=expected_peer_bytes,
                              actual_bytes=linked.get("peer_mem_bytes"))
            if (linked.get("peer_count") != mapping["rank_size"] or
                    linked["pid"] != record["pid"] or any(type(record.get(key)) is not int or record[key] != loops
                        for key in ("submitted", "completed")) or record.get("cleanup") != "complete"):
                raise Failure("RESULT_FAILED", "rank 未完成 peer window 检查、指定轮数 launch 或资源清理",
                              rank=mapping["rank"])
            if manifest["checkpoint"] == MEMORY_BOUNDARY_CHECKPOINT:
                expected_numbers = {"peer_mem_bytes": MEMORY_BOUNDARY_BYTES,
                                    "boundary_offset": MEMORY_BOUNDARY_BYTES - 32, "probe_bytes": 32}
                if (any(type(record.get(key)) is not int or record[key] != value
                            for key, value in expected_numbers.items()) or
                        any(record.get(key) is not True for key in
                            ("initial_zero", "device_initial_zero", "device_readback_ff", "host_boundary_ff", "guard_zero"))):
                    raise Failure("RESULT_FAILED", "rank 未通过 4 GiB 建链及本卡末尾 32B DataCopy 读写校验",
                                  rank=mapping["rank"])


def parse_rank_marker(output, prefix):
    markers = [line[len(prefix):] for line in output.splitlines() if line.startswith(prefix)]
    if len(markers) != 1:
        raise Failure("LOG_PARSE_FAILED", "缺少或重复 rank marker：" + prefix.strip())
    try:
        return json.loads(markers[0])
    except ValueError as error:
        raise Failure("LOG_PARSE_FAILED", "无法解析 rank marker：" + prefix.strip()) from error


def operator_kernel_shas(manifest, config):
    return {operator: manifest["artifacts"][f"lib/tilexr/kernels/tilexr_ep_{operator}_memory_server_dedup_kernel.o"]
            for operator in operator_measure.active_operators(config)}


def parse_operator_breakdowns(output, mapping, run_id, manifest, record, config, records_paths, capture_id):
    expected = set(operator_measure.active_operators(config)) if config["run_mode"] == "breakdown" else set()
    markers = {}
    for line in output.splitlines():
        if line.startswith(BREAKDOWN_MARKER):
            raise Failure("LOG_PARSE_FAILED", "C6不接受未区分算子的旧breakdown marker")
        if not line.startswith(OPERATOR_BREAKDOWN_MARKER):
            continue
        try:
            marker = json.loads(line[len(OPERATOR_BREAKDOWN_MARKER):])
        except ValueError as error:
            raise Failure("LOG_PARSE_FAILED", "C6算子breakdown marker不是有效JSON") from error
        operator = marker.get("operator") if isinstance(marker, dict) else None
        if not isinstance(operator, str) or operator not in expected or operator in markers:
            raise Failure("LOG_PARSE_FAILED", "C6算子breakdown marker重复或包含未启用算子")
        wanted = dict(rank_record(mapping, run_id, manifest), pid=record["pid"], operator=operator,
                      schema=measurements.RECORDS_SCHEMA, records_path=str(records_paths[operator]),
                      capture_id=capture_id, launches=config["warmup"] + config["loops"],
                      capture_mode="latest_launch", captured_launches=1,
                      captured_launch=config["warmup"] + config["loops"] - 1)
        if any(type(marker.get(key)) is not type(value) or marker[key] != value for key, value in wanted.items()):
            raise Failure("RESULT_FAILED", "C6算子breakdown身份/路径/批次不匹配", rank=mapping["rank"], operator=operator)
        markers[operator] = marker
    if set(markers) != expected:
        raise Failure("LOG_PARSE_FAILED", "C6缺少启用算子的breakdown marker", rank=mapping["rank"])
    return markers


def run_ranks(config, run_id, manifest, mappings, deadline, cancelled, result):
    def launch(mapping):
        rank = mapping["rank"]
        log = config.logs / run_id / f"rank-{rank}.log"
        record, communication_completed = None, False
        try:
            if cancelled.is_set():
                return None
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("EXIT_FAILED", "rank 启动前已超过节点执行期限", timeout=True)
            env = None
            argv = [sys.executable, Path(__file__), "rank", run_id, manifest["build_id"], str(rank)]
            if config.checkpoint in CHAIN_CHECKPOINTS:
                argv = [config.install / "bin" / CHAIN_PROGRAM, run_id, manifest["build_id"],
                        manifest["source"]["commit"] or "unknown",
                        *[str(mapping[key]) for key in ("host", "host_index", "local_rank", "device", "rank", "rank_size")]]
                env = dict(os.environ)
                env["TILEXR_VALIDATION_CHECKPOINT"] = config.checkpoint
                env["TILEXR_VALIDATION_BOUNDARY_PROBE"] = str(int(config.chain.get("boundary_probe", False)))
                for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "quant_mode", "operator_mode", "dispatch_quant_mode", "combine_quant_mode", "fp8_format", "loops", "warmup", "run_mode", "route_mode", "seed"):
                    env["TILEXR_VALIDATION_" + key.upper()] = str(config.chain[key])
                for key in ("combine_fixture", "combine_observation"):
                    if key in config.chain:
                        env["TILEXR_VALIDATION_" + key.upper()] = config.chain[key]
                inputs = log.parent / "inputs"
                inputs.mkdir(parents=True, exist_ok=True)
                env["TILEXR_VALIDATION_INPUT_DIR"] = str(inputs)
                env["TILEXR_COMM_ID"] = config.chain["comm_id"]
                env["TILEXR_DEMO_BARRIER_ADDR"] = config.chain["barrier_addr"]
                block_dim = "TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM"
                env.pop(block_dim, None)
                if config.chain["block_dim"] != "auto":
                    env[block_dim] = config.chain["block_dim"]
                env["LD_LIBRARY_PATH"] = str(config.install / "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
                for key in ("TILEXR_PROFILING_RECORDS_PATH", "TILEXR_PROFILING_COMBINE_RECORDS_PATH",
                            "TILEXR_PROFILING_CAPTURE_ID", "TILEXR_PROFILING_RECORDS_PER_CORE"):
                    env.pop(key, None)
                records_paths = {}
                if config.chain["run_mode"] == "breakdown":
                    records_path = log.parent / f"rank-{rank}.records.v1"
                    env.update(TILEXR_PROFILING_CAPTURE_ID=str(uuid.uuid5(uuid.NAMESPACE_URL, "tilexr-validation:" + run_id)),
                               TILEXR_PROFILING_RECORDS_PER_CORE=str(measurements.capture_capacity(
                                   config.chain["loops"], config.chain["warmup"])))
                    if config.checkpoint in COMBINE_MEASUREMENT_CHECKPOINTS:
                        for operator in operator_measure.active_operators(config.chain):
                            records_paths[operator] = log.parent / f"rank-{rank}.{operator}.records.v1"
                            key = "TILEXR_PROFILING_RECORDS_PATH" if operator == "dispatch" else "TILEXR_PROFILING_COMBINE_RECORDS_PATH"
                            env[key] = str(records_paths[operator])
                    else:
                        env["TILEXR_PROFILING_RECORDS_PATH"] = str(records_path)
                if config.chain["run_mode"] == "performance":
                    profile_dir = log.parent / f"msprof-rank-{rank}"
                    argv = [config.chain["msprof"], "--application=" + shlex.join([str(arg) for arg in argv]),
                            "--output=" + str(profile_dir)]
            output = execute(argv, log, timeout=remaining, cwd=config.root, env=env, cancelled=cancelled)
            if config.checkpoint in CHAIN_CHECKPOINTS:
                marker = DISPATCH_MARKER
                if config.checkpoint == MEMORY_BOUNDARY_CHECKPOINT:
                    marker = MEMORY_BOUNDARY_MARKER
                if config.checkpoint in COMBINE_DEVICE_CHECKPOINTS and config.chain["operator_mode"] != "dispatch":
                    marker = COMBINE_CHAIN_MARKER
                if config.chain.get("boundary_probe"):
                    marker = COMBINE_BOUNDARY_MARKER
                dispatch_invalid = any(line.startswith(DISPATCH_INVALID_MARKER) for line in output.splitlines())
                combine_invalid = any(line.startswith(COMBINE_INVALID_MARKER) for line in output.splitlines())
                if dispatch_invalid:
                    if config.checkpoint in COMBINE_DEVICE_CHECKPOINTS and config.checkpoint != COMBINE_M3_CHECKPOINT:
                        raise Failure("RESULT_FAILED", "Dispatch 输出校验失败")
                    if any(line.startswith(DISPATCH_MARKER) for line in output.splitlines()):
                        raise Failure("LOG_PARSE_FAILED", "Dispatch 同时报告 PASS 和 INVALID")
                    if config.chain["operator_mode"] == "dispatch":
                        marker = DISPATCH_INVALID_MARKER
                if combine_invalid:
                    if config.checkpoint != COMBINE_M3_CHECKPOINT or config.chain["operator_mode"] == "dispatch":
                        raise Failure("RESULT_FAILED", "unexpected Combine INVALID marker")
                    if any(line.startswith(COMBINE_CHAIN_MARKER) for line in output.splitlines()):
                        raise Failure("LOG_PARSE_FAILED", "Combine reports both PASS and INVALID")
                    marker = COMBINE_INVALID_MARKER
                record = parse_rank_marker(output, marker)
                if not isinstance(record, dict):
                    raise Failure("LOG_PARSE_FAILED", "轮次记录不是 JSON 对象")
                if marker == DISPATCH_INVALID_MARKER and record.get("validation_status") != "FAIL":
                    raise Failure("LOG_PARSE_FAILED", "INVALID marker 缺少输出校验失败状态")
                if marker == DISPATCH_MARKER and record.get("validation_status") == "FAIL":
                    raise Failure("LOG_PARSE_FAILED", "PASS marker 与输出校验失败状态矛盾")
                record["chain"] = parse_rank_marker(output, CHAIN_MARKER)
                if config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT:
                    unexpected = ((DISPATCH_MARKER, COMBINE_CHAIN_MARKER) if config.chain["boundary_probe"] else (COMBINE_BOUNDARY_MARKER,))
                    if any(line.startswith(unexpected) for line in output.splitlines()):
                        raise Failure("RESULT_FAILED", "C7探针与业务算子marker混用")
                if config.checkpoint in COMBINE_DEVICE_CHECKPOINTS and not config.chain.get("boundary_probe"):
                    has_dispatch = dispatch_invalid or any(line.startswith(DISPATCH_MARKER) for line in output.splitlines())
                    if config.chain["operator_mode"] == "both":
                        record["dispatch"] = parse_rank_marker(output, DISPATCH_INVALID_MARKER if dispatch_invalid else DISPATCH_MARKER)
                    elif config.chain["operator_mode"] == "combine" and has_dispatch:
                        raise Failure("RESULT_FAILED", "Combine 独跑意外出现 Dispatch PASS")
                    elif config.chain["operator_mode"] == "dispatch" and any(
                            line.startswith(COMBINE_CHAIN_MARKER) for line in output.splitlines()):
                        raise Failure("RESULT_FAILED", "Dispatch 独跑意外出现 Combine PASS")
                if config.checkpoint == DISPATCH_CHECKPOINT:
                    validate_rank_records([record], [mapping], run_id, manifest, config.chain,
                                          check_measurements=False, allow_invalid_output=True)
                    communication_completed = True
                if config.checkpoint in COMBINE_MEASUREMENT_CHECKPOINTS:
                    validate_rank_records([record], [mapping], run_id, manifest, config.chain, check_measurements=False, allow_invalid_output=True)
                    communication_completed = True
                    parse_operator_breakdowns(output, mapping, run_id, manifest, record, config.chain,
                                              records_paths, env.get("TILEXR_PROFILING_CAPTURE_ID"))
                    record["operator_measurements"] = {}
                    kernel_shas = operator_kernel_shas(manifest, config.chain)
                    capture_errors = []
                    for operator in operator_measure.active_operators(config.chain):
                        if config.chain["run_mode"] == "functional":
                            continue
                        try:
                            if config.chain["run_mode"] == "performance":
                                evidence = operator_measure.performance_record(profile_dir, config.chain["loops"],
                                    config.chain["warmup"], operator=operator, run_id=run_id, rank=rank,
                                    kernel_sha=kernel_shas[operator],
                                    **(dict(config=config.chain, record=record) if config.checkpoint in COMBINE_MODULE_CHECKPOINTS else {}))
                            else:
                                with profiling_lock:
                                    evidence = operator_measure.breakdown_record(records_paths[operator], config.chain, rank,
                                        env["TILEXR_PROFILING_CAPTURE_ID"], log.parent / f"profile-rank-{rank}" / operator,
                                        operator=operator, run_id=run_id, kernel_sha=kernel_shas[operator],
                                        mapping_path=config.install / ("share/tilexr/profiling/server_dedup.mapping.json" if operator == "dispatch"
                                            else "share/tilexr/profiling/server_dedup_combine.mapping.json"),
                                        **(dict(record=record) if config.checkpoint in COMBINE_MODULE_CHECKPOINTS else {}))
                            record["operator_measurements"][operator] = {config.chain["run_mode"]: evidence}
                        except (ValueError, TypeError, KeyError, OSError) as error:
                            capture_errors.extend(failure_records(error, "RESULT_FAILED", rank=rank, operator=operator))
                    if capture_errors:
                        raise Failure("RESULT_FAILED", "C6算子测量采集失败；保留其他算子已完成报告", additional_failures=capture_errors)
                elif config.chain["run_mode"] == "performance":
                    try:
                        record["performance"] = measurements.performance_record(profile_dir,
                            config.chain["loops"], config.chain["warmup"])
                    except (ValueError, OSError) as error:
                        raise Failure("RESULT_FAILED", f"msprof 测量记录无效：{error}", rank=rank) from error
                elif config.chain["run_mode"] == "breakdown":
                    breakdown = parse_rank_marker(output, BREAKDOWN_MARKER)
                    wanted = rank_record(mapping, run_id, manifest)
                    if (not isinstance(breakdown, dict) or any(breakdown.get(key) != value for key, value in wanted.items())
                            or breakdown.get("pid") != record.get("pid")):
                        raise Failure("RESULT_FAILED", "breakdown 的 rank/运行身份不一致", rank=rank)
                    if (breakdown.get("schema") != measurements.RECORDS_SCHEMA or
                            breakdown.get("records_path") != str(records_path) or
                            breakdown.get("capture_id") != env["TILEXR_PROFILING_CAPTURE_ID"] or
                            breakdown.get("launches") != config.chain["warmup"] + config.chain["loops"] or
                            breakdown.get("capture_mode") != "latest_launch" or
                            breakdown.get("captured_launches") != 1 or
                            breakdown.get("captured_launch") != config.chain["warmup"] + config.chain["loops"] - 1):
                        raise Failure("RESULT_FAILED", "breakdown capture identity mismatch", rank=rank)
                    try:
                        # Device processes run concurrently; decode one large capture at a time.
                        with profiling_lock:
                            record["breakdown"] = measurements.breakdown_record(records_path, config.chain, rank,
                                env["TILEXR_PROFILING_CAPTURE_ID"], log.parent / f"profile-rank-{rank}",
                                config.install / "share/tilexr/profiling/server_dedup.mapping.json")
                    except (ValueError, OSError) as error:
                        raise Failure("RESULT_FAILED", f"standard profiling capture failed: {error}; communication completed",
                                      rank=rank) from error
            else:
                record = parse_rank_marker(output, DRY_RUN_MARKER)
            validate_rank_records([record], [mapping], run_id, manifest, config.chain, allow_invalid_output=True)
            return record
        except Exception as error:
            capture = dict(communication_completed=True) if communication_completed else {}
            if communication_completed and config.chain["run_mode"] == "breakdown":
                if config.checkpoint in COMBINE_MEASUREMENT_CHECKPOINTS:
                    capture.update(operator_records_paths={operator: str(path) for operator, path in records_paths.items()},
                                   report_dir=str(log.parent / f"profile-rank-{rank}"))
                else:
                    capture.update(profile_status=record.get("breakdown", {}).get("status", "unavailable"),
                                   records_path=str(records_path), report_dir=str(log.parent / f"profile-rank-{rank}"))
            failures = failure_records(error, "LAUNCH_FAILED", rank=rank, log=str(log), **capture)
            # 保留第一个观察到的失败；其他 rank 的取消/清理错误作为后续证据。
            with error_lock:
                if communication_completed:
                    result.setdefault("profiling_failures", []).append(dict(rank=rank, communication=record, **capture))
                result["errors"].extend(failures)
                cancelled.set()
            return None

    result["ranks"] = []
    error_lock = threading.Lock()
    profiling_lock = threading.Lock()
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        futures = [pool.submit(launch, mapping) for mapping in mappings]
        for future in concurrent.futures.as_completed(futures):
            record = future.result()
            if record is not None:
                result["ranks"].append(record)
    result["ranks"].sort(key=lambda record: record["rank"])
    # Node status describes execution/capture; output correctness is reported
    # separately so a completed experiment cannot cancel the other nodes.
    result["validation_status"] = "FAIL" if any(
        record.get("validation_status") == "FAIL" for record in result["ranks"]) else (
        "PASS" if not result["errors"] else "UNKNOWN")


def node_tests(manifest):
    # The manifest is the installed catalog, not a prerequisite suite for NPU runs.
    if manifest["checkpoint"] == CHECKPOINT:
        return manifest["tests"]  # Preserve the explicit legacy ABI/Host checkpoint.
    if manifest["checkpoint"] == EMPTY_KERNEL_CHECKPOINT:
        return [test for test in manifest["tests"] if test["name"] == REGISTRATION_TEST]
    return []


def run_validation_test(config, test, log, timeout, *, device=None, cancelled=None):
    name = test["name"]
    argv = [sys.executable if value == "@python" else value if value.startswith("--") else str(config.root / value)
            for value in test["argv"]]
    env = None
    if name in (REGISTRATION_TEST, COMBINE_REGISTRATION_TEST):
        if device is None or not str(device).isdigit() or not 0 <= int(device) <= 255:
            raise ValueError("设备注册检查缺少有效的 device ID（0..255）")
        env = dict(os.environ, TILEXR_EP_SERVER_DEDUP_TEST_DEVICE=str(device))
        env["LD_LIBRARY_PATH"] = str(config.install / "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
    path = log / (name + ".log")
    output = execute(argv, path, timeout=timeout, cwd=config.root, env=env, cancelled=cancelled)
    prefix = "test_tilexr_ep_server_dedup_"
    marker = None
    if name == REGISTRATION_TEST:
        marker, suffix = "EP_SERVER_DEDUP_EMPTY_KERNEL_", " test=registration"
    elif name in (COMBINE_LAUNCH_TEST, COMBINE_REGISTRATION_TEST):
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_EMPTY_KERNEL_", " test=" + name.removeprefix(prefix + "combine_")
    elif name in (COMBINE_CHAIN_TEST, COMBINE_DELAY_TEST):
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_CHAIN_", " test=" + ("delay" if name == COMBINE_DELAY_TEST else "lifecycle")
    elif name == COMBINE_BOUNDARY_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_BOUNDARY_HOST_", " test=lifecycle"
    elif name in (COMBINE_BOUNDARY_KERNEL_TEST, COMBINE_BOUNDARY_LAUNCH_TEST):
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_BOUNDARY_" + ("KERNEL_" if name == COMBINE_BOUNDARY_KERNEL_TEST else "LAUNCH_"), ""
    elif name == COMBINE_PROFILE_KERNEL_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_PROFILE_KERNEL_", ""
    elif name == COMBINE_PROFILE_LAUNCH_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_EMPTY_KERNEL_", " test=launch"
    elif name == COMBINE_PROFILE_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_PROFILE_", " test=lifecycle"
    elif name == COMBINE_PROFILE_REGIONS_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_PROFILE_REGIONS_", ""
    elif name == COMBINE_M3_CONSUMER_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_M3_", " test=lifecycle"
    elif name == COMBINE_M3_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_M3_", ""
    elif name == COMBINE_M12_CONSUMER_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_M12_", " test=lifecycle"
    elif name == COMBINE_CONFIG_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_CONFIG_", ""
    elif name == COMBINE_M12_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_M12_", ""
    elif name == COMBINE_MULTI_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_MULTI_", " test=lifecycle"
    elif name == COMBINE_DRY_RUN_TEST:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_DRYRUN_", " test=configuration"
    elif name in COMBINE_ABI_HOST_TESTS:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_ABI_HOST_", " test=" + name.removeprefix(prefix + "combine_")
    elif name.startswith(prefix):
        marker, suffix = "EP_SERVER_DEDUP_ABI_HOST_", " test=" + name[len(prefix):]
    elif name == "test_tilexr_ep_kernel_launch" and config.checkpoint == EMPTY_KERNEL_CHECKPOINT:
        marker, suffix = "EP_SERVER_DEDUP_EMPTY_KERNEL_", " test=launch"
    if marker:
        markers = [line for line in output.splitlines() if line.startswith(marker)]
        if markers == [marker + "FAIL" + suffix]:
            raise Failure("RESULT_FAILED", "测试完成但结果校验失败", log=str(path))
        if markers != [marker + "PASS" + suffix]:
            raise Failure("LOG_PARSE_FAILED", "缺少或无法解析唯一的 PASS/FAIL marker", log=str(path))


def host_validate(config, run_id=None):
    run_id = run_id or "host-" + uuid.uuid4().hex
    log = config.logs / run_id
    result = {"run_id": run_id, "host": socket.gethostname(), "checkpoint": config.checkpoint,
              "mode": config.mode, "status": "FAIL", "tests": [], "errors": [],
              "scope": "本机已安装的 Host/Python 回归；不包含设备注册、NPU launch 或通信验证。"}
    if config.checkpoint in COMBINE_TESTS:
        result.update(scope="Combine C1：公共 ABI、Host 参数校验及布局；不包含 NPU launch 或通信验证。",
                      build_timeout_seconds=900, validation_timeout_seconds=config.timeout,
                      per_test_timeout_seconds=120)
    if config.checkpoint == COMBINE_EMPTY_KERNEL_CHECKPOINT:
        result.update(scope="Combine C2：C1回归、生产launch替身及本机A5空kernel安装/注册/64核提交/同步；不包含通信或业务数据验证。",
                      device=os.environ.get("TILEXR_EP_SERVER_DEDUP_TEST_DEVICE", "0"))
    if config.checkpoint == COMBINE_CHAIN_CHECKPOINT:
        result.update(scope="Combine C4 Host回归：C1/C3、同consumer链路替身及空kernel等待；不含真实NPU提交。")
    if config.checkpoint == COMBINE_MULTI_CHECKPOINT:
        result.update(scope="Combine C5 Host回归：C1/C3/C4、连续批次/预热/选窗/生命周期及空kernel等待；不含真实NPU提交。")
    if config.checkpoint in COMBINE_MEASUREMENT_CHECKPOINTS:
        result.update(scope="Combine C6 Host回归：C1/C3/C4/C5、独立记录区及三模式profiling；不含真实NPU提交。")
    if config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT:
        result.update(scope="Combine C7 Host回归：C1/C3/C4/C5/C6、边界探针注册/kernel与consumer；不含真实NPU提交。")
    if config.checkpoint == COMBINE_M3_CHECKPOINT:
        result.update(scope="Combine P2.2 Host: M3 single-launch oracle, module profiling and public contracts; no device execution.")
    if config.checkpoint == COMBINE_M12_CHECKPOINT:
        result.update(scope="Combine P2.1 Host回归：M0/M1/M2契约、consumer与故障传播；不含真实NPU执行。")
    if config.checkpoint == COMBINE_DRY_RUN_CHECKPOINT:
        result.update(scope="Combine C3：C1回归、同一consumer的三种算子模式、独立量化开关及rank映射；无CANN/SSH/设备分配/communicator/launch。")
    started = time.monotonic()
    cancelled = threading.Event()
    handlers = {sig: signal.signal(sig, lambda signum, frame: cancelled.set())
                for sig in (signal.SIGTERM, signal.SIGHUP)}
    current_test = None
    try:
        manifest = verify_build(config, check_run_mode=False)
        result.update(build_id=manifest["build_id"], commit=manifest["source"]["commit"],
                      source_provenance=manifest["source"]["provenance"],
                      source_working_tree_dirty=manifest["source"].get("working_tree_dirty"),
                      build_logs=str(config.logs / ("build-" + manifest["build_id"])))
        tests = [test for test in manifest["tests"] if test["name"] != REGISTRATION_TEST]
        names = [test["name"] for test in tests]
        expected = COMBINE_TESTS.get(config.checkpoint)
        if expected and set(names) != expected:
            raise ValueError(f"Combine 安装包测试集不完整：缺少 {sorted(expected - set(names))}；多余 {sorted(set(names) - expected)}")
        if not names or len(names) != len(set(names)):
            raise ValueError("安装包没有 Host/Python 测试或测试名称重复，请检查构建 manifest")
        result["expected_tests"] = names
        for test in tests:
            current_test = test["name"]
            if cancelled.is_set():
                raise InterruptedError("Host/Python 回归已取消")
            budget = config.timeout
            if config.checkpoint in COMBINE_TESTS:
                budget = min(120, started + config.timeout - time.monotonic())
                if budget <= 0:
                    raise Failure("EXIT_FAILED", "Combine 验证超过总期限", timeout=True)
            device = result.get("device") if current_test == COMBINE_REGISTRATION_TEST else None
            run_validation_test(config, test, log, budget, device=device, cancelled=cancelled)
            result["tests"].append({"name": current_test, "status": "PASS"})
            current_test = None
        if cancelled.is_set():
            raise InterruptedError("Host/Python 回归已取消")
        result["status"] = "PASS"
    except (Exception, KeyboardInterrupt) as error:
        kind = "EXIT_FAILED" if isinstance(error, (InterruptedError, KeyboardInterrupt)) else "CONFIG_FAILED"
        result["errors"] = failure_records(error, kind, **({"test": current_test} if current_test else {}))
        if current_test:
            result["tests"].append({"name": current_test, "status": "FAIL",
                                    "failure_type": result["errors"][0]["type"]})
    finally:
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
        config.cleanup_failed = any(error["type"] == "CLEANUP_FAILED" for error in result["errors"])
        result["cleanup"] = "failed" if config.cleanup_failed else "complete"
        result["elapsed_seconds"] = time.monotonic() - started
        write_json(log / "summary.json", result)
    for error in result["errors"]:
        report_failure(error)
    print(f"HOST_VALIDATION_{result['status']} commit={result.get('commit', 'unknown')} "
          f"build_id={result.get('build_id', 'unknown')} "
          f"tests={sum(test['status'] == 'PASS' for test in result['tests'])}/{len(result.get('expected_tests', []))}", flush=True)
    print(f"结果：{log / 'summary.json'}\n范围：{result['scope']}", flush=True)
    return 0 if result["status"] == "PASS" else FAILURE_CODES[result["errors"][0]["type"]]


COMBINE_CHAIN_CASES = (
    ("combine-bf16", "combine", "bf16", 0, "e4m3"),
    ("combine-fp16", "combine", "fp16", 0, "e4m3"),
    ("both-bf16", "both", "bf16", 0, "e4m3"),
    ("both-fp16", "both", "fp16", 0, "e4m3"),
    ("both-bf16-mxfp8-e4m3", "both", "bf16", 4, "e4m3"),
    ("both-fp16-mxfp8-e5m2", "both", "fp16", 4, "e5m2"),
)


COMBINE_BOUNDARY_CASES = (
    ("boundary", "combine", "bf16", 0, "e4m3"),
    ("dispatch-bf16", "dispatch", "bf16", 0, "e4m3"),
    ("dispatch-fp16", "dispatch", "fp16", 0, "e4m3"),
)


COMBINE_MULTI_CASES = (
    ("dispatch-bf16", "dispatch", "bf16", 0, "e4m3"),
    ("dispatch-fp16", "dispatch", "fp16", 0, "e4m3"),
) + COMBINE_CHAIN_CASES


def combine_chain_commit_validate(config, run_id):
    """Run one checkpoint's bounded matrix through the normal sync/run path."""
    multi = config.checkpoint == COMBINE_MULTI_CHECKPOINT
    boundary = config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT
    cases = COMBINE_BOUNDARY_CASES if boundary else COMBINE_MULTI_CASES if multi else COMBINE_CHAIN_CASES
    checkpoint_label = "C7" if boundary else "C5" if multi else "C4"
    loops, warmup = (5, 2) if multi else (1, 0)
    log = config.logs / run_id
    summary = dict(run_id=run_id, checkpoint=config.checkpoint, mode=config.mode, status="FAIL",
                   cases=[], errors=[], expected_cases=len(cases),
                   scope="Combine C4：C1/C3回归、Host生命周期及50us空kernel；前2台固定8P，Combine独跑及Dispatch→空Combine共6个单轮用例；不验Combine通信或业务数值。")
    if multi:
        summary["scope"] = ("Combine C5：C1/C3/C4/C5 Host回归及50us空kernel；前2台固定8P，"
                            "三种算子模式共8个串行用例，每例2轮预热+5轮正式调用；"
                            "批末同步、各自magic/选窗和最终Dispatch校验；不验Combine通信或业务数值。")
    if boundary:
        summary["scope"] = ("Combine C7：C1-C6 Host回归及边界探针；前2台固定8P，独立8GiB双窗8点探针、"
                            "BF16/FP16真实Dispatch多轮回归共3例；避开runtime保留区，不验Combine业务数据。")
    original_environment = dict(os.environ)
    stage = "host-regression"
    case_config = None
    try:
        manifest = verify_build(config, check_run_mode=False)
        summary.update(build_id=manifest["build_id"], commit=manifest["source"]["commit"],
                       source_provenance=manifest["source"]["provenance"],
                       source_working_tree_dirty=manifest["source"].get("working_tree_dirty"),
                       build_timeout_seconds=900, validation_timeout_seconds=config.timeout,
                       per_test_timeout_seconds=120, per_case_timeout_seconds=config.timeout,
                       build_logs=str(config.logs / ("build-" + manifest["build_id"])))
        if boundary:
            summary["source_content_sha256"] = manifest["source"]["content_sha256"]
            summary["manifest"] = str(log / "build-manifest.json")
            write_json(Path(summary["manifest"]), manifest)
            os.environ["TILEXR_VALIDATION_BOUNDARY_PROBE"] = "0"
        host_id = run_id + "/host-regression"
        code = host_validate(config, run_id=host_id)
        summary["host_regression"] = str(config.logs / host_id / "summary.json")
        if code:
            raise Failure("RESULT_FAILED", checkpoint_label + " Host 回归失败；未启动设备用例")
        # The acceptance matrix owns case parameters. Deployment paths, SSH user
        # and CANN come from the same configuration used by normal sync/run.
        for key in list(os.environ):
            if key.startswith("TILEXR_VALIDATION_") and key.removeprefix("TILEXR_VALIDATION_") in (
                    "RUN_MODE", "LOOPS", "WARMUP", "BS", "H", "TOPK", "MOE_EXPERT_NUM", "DTYPE",
                    "QUANT_MODE", "DISPATCH_QUANT_MODE", "COMBINE_QUANT_MODE", "OPERATOR_MODE",
                    "FP8_FORMAT", "BLOCK_DIM", "CASE_ANALYSIS", "ROUTE_MODE", "SEED", "SERVERS", "BOUNDARY_PROBE"):
                os.environ.pop(key)
        os.environ.pop("TILEXR_DIRECT_HOSTS_JSON", None)
        os.environ.update(TILEXR_VALIDATION_RUN_MODE="functional", TILEXR_VALIDATION_LOOPS=str(loops),
                          TILEXR_VALIDATION_WARMUP=str(warmup), TILEXR_VALIDATION_SERVERS="2",
                          TILEXR_VALIDATION_TIMEOUT=str(config.timeout),
                          TILEXR_VALIDATION_BS="96", TILEXR_VALIDATION_H="7168", TILEXR_VALIDATION_TOPK="6",
                          TILEXR_VALIDATION_MOE_EXPERT_NUM="96", TILEXR_VALIDATION_COMBINE_QUANT_MODE="0",
                          TILEXR_VALIDATION_OPERATOR_MODE="combine", TILEXR_VALIDATION_BLOCK_DIM="auto",
                          TILEXR_VALIDATION_ROUTE_MODE="balance_shift", TILEXR_VALIDATION_SEED="2026",
                          TILEXR_VALIDATION_CASE_ANALYSIS="0")
        stage = "sync"
        case_config = Config("run")
        selected_hosts = case_config.hosts()
        summary.update(expected_hosts=selected_hosts, rank_size=len(selected_hosts) * 8)
        sync(case_config)
        print(f"{checkpoint_label} 设备矩阵共 {len(cases)} 个串行用例；各例轮次见START行，首个失败即停止。", flush=True)
        for index, (name, operator, dtype, quant, fp8) in enumerate(cases, 1):
            stage = name
            if boundary:
                loops, warmup = (1, 0) if name == "boundary" else (5, 2)
                os.environ.update(TILEXR_VALIDATION_BOUNDARY_PROBE="1" if name == "boundary" else "0",
                                  TILEXR_VALIDATION_LOOPS=str(loops), TILEXR_VALIDATION_WARMUP=str(warmup))
            os.environ.update(TILEXR_VALIDATION_OPERATOR_MODE=operator, TILEXR_VALIDATION_DTYPE=dtype,
                              TILEXR_VALIDATION_QUANT_MODE=str(quant), TILEXR_VALIDATION_DISPATCH_QUANT_MODE=str(quant),
                              TILEXR_VALIDATION_FP8_FORMAT=fp8,
                              TILEXR_VALIDATION_RESULT_ROOT=str(log / "cases" / name))
            case_config = Config("run")
            print(f"[{index}/{len(cases)}] START {name} operator={operator} dtype={dtype} "
                  f"dispatch_quant={quant} combine_quant=0 fp8={fp8} loops={loops} warmup={warmup} "
                  f"结果目录：{case_config.logs}", flush=True)
            try:
                code = run(case_config)
            except (Exception, KeyboardInterrupt) as error:
                errors = failure_records(error, "RESULT_FAILED", stage=name)
                case_config.last_summary_path = case_config.logs / "failure.json"
                write_json(case_config.last_summary_path, dict(status="FAIL", checkpoint=config.checkpoint,
                    chain_configuration=dict(case_config.chain), rank_size=16, completed_ranks=0, errors=errors))
                code = FAILURE_CODES[errors[0]["type"]]
            report = read_json(case_config.last_summary_path)
            passed = (code == 0 and report.get("status") == "PASS" and
                      report.get("rank_size") == 16 and report.get("completed_ranks") == 16 and
                      (not boundary or report.get("build_id") == manifest["build_id"]))
            summary["cases"].append(dict(name=name, status="PASS" if passed else "FAIL",
                                         summary=str(case_config.last_summary_path),
                                         configuration=dict(case_config.chain),
                                         completed_ranks=report.get("completed_ranks", 0)))
            print(f"[{index}/{len(cases)}] {'PASS' if passed else 'FAIL'} {name} "
                  f"结果：{case_config.last_summary_path}", flush=True)
            if not passed:
                raise Failure("RESULT_FAILED", checkpoint_label + " 用例失败或缺少完整16 rank：" + name)
        summary["status"] = "PASS"
    except (Exception, KeyboardInterrupt) as error:
        summary["errors"] = failure_records(error, "RESULT_FAILED", stage=stage)
        for failure in summary["errors"]:
            report_failure(failure)
    finally:
        os.environ.clear()
        os.environ.update(original_environment)
        config.cleanup_failed = (getattr(config, "cleanup_failed", False) or
                                 getattr(case_config, "cleanup_failed", False))
        summary["cleanup"] = "failed" if config.cleanup_failed else "complete"
        write_json(log / "summary.json", summary)
    print(f"结果：{log / 'summary.json'}\n范围：{summary['scope']}", flush=True)
    return 0 if summary["status"] == "PASS" else FAILURE_CODES[summary["errors"][0]["type"]]


def combine_profile_commit_validate(config, run_id):
    """Keep OFF/ON builds and all six case reports under one acceptance run."""
    log = config.logs / run_id
    summary = dict(run_id=run_id, checkpoint=config.checkpoint, mode=config.mode, status="FAIL",
                   phases=[], cases=[], expected_cases=6, errors=[], build_timeout_seconds=900,
                   validation_timeout_seconds=config.timeout, per_case_timeout_seconds=config.timeout,
                   scope="Combine C6：独立profiling OFF/ON构建，各跑dispatch/combine/both；共6组16P，"
                         "每组2轮预热+5轮正式调用；独立测量/trace，空Combine算法字节0且带宽N/A；不验Combine通信或业务数值。")
    original_environment = dict(os.environ)
    stage = "build-performance"
    active_config = None
    try:
        # The gate owns the case matrix; deployment and msprof paths remain caller configuration.
        for key in ("RUN_MODE", "LOOPS", "WARMUP", "BS", "H", "TOPK", "MOE_EXPERT_NUM", "DTYPE",
                    "QUANT_MODE", "DISPATCH_QUANT_MODE", "COMBINE_QUANT_MODE", "OPERATOR_MODE", "FP8_FORMAT",
                    "BLOCK_DIM", "CASE_ANALYSIS", "ROUTE_MODE", "SEED", "SERVERS"):
            os.environ.pop("TILEXR_VALIDATION_" + key, None)
        os.environ.pop("TILEXR_DIRECT_HOSTS_JSON", None)
        os.environ.update(TILEXR_VALIDATION_LOOPS="5", TILEXR_VALIDATION_WARMUP="2", TILEXR_VALIDATION_SERVERS="2",
                          TILEXR_VALIDATION_TIMEOUT=str(config.timeout), TILEXR_VALIDATION_BS="96",
                          TILEXR_VALIDATION_H="7168", TILEXR_VALIDATION_TOPK="6", TILEXR_VALIDATION_MOE_EXPERT_NUM="96",
                          TILEXR_VALIDATION_DTYPE="bf16", TILEXR_VALIDATION_QUANT_MODE="0",
                          TILEXR_VALIDATION_DISPATCH_QUANT_MODE="0", TILEXR_VALIDATION_COMBINE_QUANT_MODE="0",
                          TILEXR_VALIDATION_FP8_FORMAT="e4m3", TILEXR_VALIDATION_BLOCK_DIM="auto",
                          TILEXR_VALIDATION_ROUTE_MODE="balance_shift", TILEXR_VALIDATION_SEED="2026",
                          TILEXR_VALIDATION_CASE_ANALYSIS="0")
        for mode in ("performance", "breakdown"):
            stage = "build-" + mode
            os.environ.update(TILEXR_VALIDATION_RESULT_ROOT=str(config.logs), TILEXR_VALIDATION_RUN_MODE=mode,
                              TILEXR_VALIDATION_OPERATOR_MODE="dispatch",
                              TILEXR_VALIDATION_BREAKDOWN="1" if mode == "breakdown" else "0")
            active_config = Config("build")
            phase = dict(run_mode=mode, profiling=active_config.profiling, status="FAIL")
            summary["phases"].append(phase)
            try:
                build(active_config)
            finally:
                phase.update(build_id=getattr(active_config, "build_id", None),
                             build_logs=str(config.logs / ("build-" + getattr(active_config, "build_id", "unknown"))))
            manifest = verify_build(active_config, check_run_mode=False)
            fingerprint = manifest["source"].get("content_sha256")
            if not isinstance(fingerprint, str) or not re.fullmatch(r"[0-9a-f]{64}", fingerprint):
                raise Failure("BUILD_FAILED", "C6构建缺少源码内容摘要")
            identity = dict(commit=manifest["source"]["commit"], content_sha256=fingerprint)
            if "source_identity" in summary and summary["source_identity"] != identity:
                raise Failure("BUILD_FAILED", "C6 OFF/ON源码或commit不一致，拒绝混合版本验收")
            summary["source_identity"] = identity
            phase["source_identity"] = identity
            phase["build_directory"] = str(validation_build_directory(active_config))
            phase["manifest"] = str(log / "builds" / (mode + ".manifest.json"))
            write_json(Path(phase["manifest"]), manifest)
            summary.update(commit=manifest["source"]["commit"], source_provenance=manifest["source"]["provenance"])
            stage = "host-" + mode
            host_id = run_id + "/host-" + mode
            phase["host_regression"] = str(config.logs / host_id / "summary.json")
            if host_validate(active_config, run_id=host_id):
                raise Failure("RESULT_FAILED", "C6 Host回归失败；未启动本phase设备用例")
            stage = "sync-" + mode
            active_config = Config("run")
            hosts = active_config.hosts()
            summary.update(expected_hosts=hosts, rank_size=len(hosts) * 8)
            sync(active_config)
            for operator in ("dispatch", "combine", "both"):
                name = mode + "-" + operator
                index = len(summary["cases"]) + 1
                stage = name
                os.environ.update(TILEXR_VALIDATION_OPERATOR_MODE=operator,
                                  TILEXR_VALIDATION_RESULT_ROOT=str(log / "cases" / name))
                active_config = Config("run")
                print(f"[{index}/6] START {name} dtype=bf16 dispatch_quant=0 combine_quant=0 "
                      f"loops=5 warmup=2 build_id={manifest['build_id']} 结果目录：{active_config.logs}", flush=True)
                try:
                    code = run(active_config)
                except (Exception, KeyboardInterrupt) as error:
                    active_config.last_summary_path = active_config.logs / "failure.json"
                    write_json(active_config.last_summary_path, dict(status="FAIL", checkpoint=config.checkpoint,
                        build_id=manifest["build_id"], errors=failure_records(error, "RESULT_FAILED", stage=stage)))
                    code = 1
                report = read_json(active_config.last_summary_path)
                passed = (code == 0 and report.get("status") == "PASS" and report.get("rank_size") == 16
                          and report.get("completed_ranks") == 16 and report.get("build_id") == manifest["build_id"])
                summary["cases"].append(dict(name=name, status="PASS" if passed else "FAIL",
                    build_id=manifest["build_id"], summary=str(active_config.last_summary_path),
                    configuration=dict(active_config.chain), completed_ranks=report.get("completed_ranks", 0)))
                print(f"[{index}/6] {'PASS' if passed else 'FAIL'} {name} 结果：{active_config.last_summary_path}", flush=True)
                if not passed:
                    raise Failure("RESULT_FAILED", "C6用例失败、构建不匹配或缺少完整16 rank：" + name)
            phase["status"] = "PASS"
        summary["status"] = "PASS"
    except (Exception, KeyboardInterrupt) as error:
        summary["errors"] = failure_records(error, "BUILD_FAILED" if stage.startswith("build-") else "RESULT_FAILED", stage=stage)
        if "commit" not in summary:
            try:
                source = inventory(config.root)
            except Exception:
                source = dict(commit=None, provenance="unavailable")
            summary.update(commit=source["commit"], source_provenance=source["provenance"])
        for failure in summary["errors"]:
            report_failure(failure)
    finally:
        os.environ.clear()
        os.environ.update(original_environment)
        config.cleanup_failed = (getattr(config, "cleanup_failed", False) or getattr(active_config, "cleanup_failed", False)
                                 or any(error["type"] == "CLEANUP_FAILED" for error in summary["errors"]))
        summary["cleanup"] = "failed" if config.cleanup_failed else "complete"
        write_json(log / "summary.json", summary)
    print(f"结果：{log / 'summary.json'}\n范围：{summary['scope']}", flush=True)
    return 0 if summary["status"] == "PASS" else FAILURE_CODES[summary["errors"][0]["type"]]


def combine_m3_cases():
    """M3 has no result consumer yet: every mode is one launch, no warmup."""
    cases = []
    for gate, mode in (("F", "functional"), ("P", "performance"), ("B", "breakdown")):
        for dtype in ("bf16", "fp16"):
            for operator in ("combine", "both"):
                cases.append(dict(name=f"{gate}-{operator}-{dtype}", gate=gate, run_mode=mode,
                                  loops=1, warmup=0, operator_mode=operator, dtype=dtype))
    for dtype in ("bf16", "fp16"):
        cases.append(dict(name=f"regression-dispatch-{dtype}", gate="regression", run_mode="functional",
                          loops=1, warmup=0, operator_mode="dispatch", dtype=dtype))
        cases.append(dict(name=f"connection-both-{dtype}-mxfp8", gate="connection", run_mode="functional",
                          loops=1, warmup=0, operator_mode="both", dtype=dtype, dispatch_quant_mode=4,
                          fp8_format="e4m3" if dtype == "bf16" else "e5m2"))
    return sorted(cases, key=lambda case: case["run_mode"] == "breakdown")


def combine_m12_cases():
    """The fixed acceptance manifest; diagnostic cases never replace base gates."""
    cases = []
    for gate, mode, loops, warmup in (("F1", "functional", 1, 0), ("F2", "functional", 5, 2),
                                       ("P", "performance", 5, 2), ("B", "breakdown", 5, 2)):
        for dtype in ("bf16", "fp16"):
            for operator in ("combine", "both"):
                cases.append(dict(name=f"{gate}-{operator}-{dtype}", gate=gate, run_mode=mode, loops=loops,
                                  warmup=warmup, operator_mode=operator, dtype=dtype))
    for dtype in ("bf16", "fp16"):
        cases.append(dict(name=f"regression-dispatch-{dtype}", gate="regression", run_mode="functional",
                          loops=5, warmup=2, operator_mode="dispatch", dtype=dtype))
        cases.append(dict(name=f"connection-both-{dtype}-mxfp8", gate="connection", run_mode="functional",
                          loops=5, warmup=2, operator_mode="both", dtype=dtype, dispatch_quant_mode=4,
                          fp8_format="e4m3" if dtype == "bf16" else "e5m2"))
    for fixture, bs, observation in (("sparse", 4, "per-round"), ("empty", 16, "batch"),
                                      ("sparse", 512, "per-round")):
        cases.append(dict(name=f"edge-{fixture}-bs{bs}", gate="edges", run_mode="functional",
                          loops=1 if fixture == "empty" else 5, warmup=0 if fixture == "empty" else 2,
                          operator_mode="combine", dtype="bf16", combine_fixture=fixture,
                          combine_observation=observation, bs=bs, h=516, topk=2, moe_expert_num=32))
    # An OFF package is built/deployed once, then the ON package. Within each
    # package preserve the manifest order and do not restart failed cases.
    return sorted(cases, key=lambda case: case["run_mode"] == "breakdown")


def combine_m12_defaults():
    return dict(bs=96, h=7168, topk=6, moe_expert_num=96, loops=5, warmup=2, dtype="bf16",
                dispatch_quant_mode=0, combine_quant_mode=0, operator_mode="combine", fp8_format="e4m3",
                block_dim="auto", route_mode="balance_shift", seed=2026, case_analysis=0,
                servers=2, boundary_probe=0, combine_fixture="balanced", combine_observation="batch")


def load_combine_m12_resume(config, cases, path, log):
    """Explicitly reuse the reported OFF run and its already-built ON companion.

    Old manifests lack per-file digests, so the source mismatch cannot be repaired
    retrospectively. Keep it visible; never rebuild one side under this exception.
    """
    path = Path(path).resolve()
    previous = read_json(path)

    def require(condition, message):
        if not condition:
            raise Failure("RESULT_FAILED", "P2.1续跑拒绝：" + message)

    require(previous.get("checkpoint") == config.checkpoint and previous.get("mode") == "production"
            and previous.get("status") == "FAIL" and previous.get("cleanup") == "complete",
            "原报告必须为本检查点且已完成清理的失败运行")
    require(previous.get("case_manifest") == cases, "原验收矩阵与当前不一致")
    errors = previous.get("errors", [])
    require(len(errors) == 1 and errors[0].get("type") == "BUILD_FAILED"
            and errors[0].get("stage") == "build-profile-on"
            and errors[0].get("message") == "P2.1 OFF/ON源码不一致，拒绝混合版本验收",
            "仅支持19项OFF通过后在ON源码一致性检查处停止的运行")
    hosts = config.hosts()[:2]
    require(previous.get("rank_size") == 16 and previous.get("expected_hosts") == hosts,
            "host/device范围已变化")
    phases = previous.get("phases", [])
    require(len(phases) == 2 and phases[0].get("package") == "profile-off"
            and phases[0].get("status") == "PASS" and phases[1].get("package") == "profile-on",
            "缺少原OFF完成阶段或ON构建身份")
    packages = {}
    for profiling, phase in zip((False, True), phases):
        os.environ["TILEXR_VALIDATION_BREAKDOWN"] = str(int(profiling))
        package_config = Config("build")
        manifest = verify_build(package_config, check_run_mode=False)
        require(manifest["build_id"] == phase.get("build_id")
                and manifest["server_dedup_profiling"] is profiling,
                "原安装包被替换或打点模式不匹配；续跑不重建安装包")
        packages[phase["package"]] = manifest
    off = packages["profile-off"]
    require(read_json(phases[0]["manifest"]) == off, "OFF安装包与原报告保存的manifest不一致")
    host_report = read_json(phases[0]["host_regression"])
    require(host_report.get("status") == "PASS" and host_report.get("build_id") == off["build_id"]
            and host_report.get("checkpoint") == config.checkpoint, "原OFF Host检查未通过或身份不匹配")
    rows = previous.get("cases", [])
    off_cases = [case for case in cases if case["run_mode"] != "breakdown"]
    require(len(rows) == len(off_cases) == 19, "原报告必须包含完整19项OFF结果")
    reports = []
    for case, row in zip(off_cases, rows):
        require(row.get("name") == case["name"] and row.get("gate") == case["gate"]
                and row.get("status") == "PASS" and row.get("completed_ranks") == 16
                and row.get("build_id") == off["build_id"], "原用例缺失、失败或身份不匹配：" + case["name"])
        expected = dict(combine_m12_defaults(), **{k: v for k, v in case.items() if k not in ("name", "gate")})
        actual = row.get("configuration", {})
        require(all(actual.get(k) == v for k, v in expected.items() if k not in ("servers", "boundary_probe")),
                "原用例参数不匹配：" + case["name"])
        report = read_json(row["summary"])
        require(report.get("status") == "PASS" and report.get("checkpoint") == config.checkpoint
                and report.get("build_id") == off["build_id"] and report.get("rank_size") == 16
                and report.get("completed_ranks") == 16 and report.get("expected_hosts") == hosts
                and report.get("chain_configuration") == actual and not report.get("errors"),
                "原用例summary未完整通过或与总报告不符：" + case["name"])
        reports.append(report)
    # Snapshot evidence only after all prerequisites pass; never alter old reports.
    write_json(log / "reused/original-summary.json", previous)
    write_json(log / "reused/off-host-summary.json", host_report)
    reused = []
    for row, report in zip(rows, reports):
        saved = log / "reused/cases" / (row["name"] + ".summary.json")
        write_json(saved, report)
        reused.append(dict(row, reused=True, origin_summary=row["summary"], summary=str(saved),
                           summary_sha256=digest(saved)))
    metadata = dict(original_summary=str(path), original_summary_sha256=digest(path),
                    runner_workflow_sha256=digest(Path(__file__)),
                    source_consistency="historical-mismatch-explicit-resume",
                    original_errors=errors, reused_cases=19, new_cases=4,
                    artifact_policy="reuse-original-off-and-on-packages-no-rebuild",
                    source_identities={key: dict(commit=m["source"]["commit"],
                        content_sha256=m["source"]["content_sha256"]) for key, m in packages.items()})
    return previous, packages, reused, metadata


def combine_m12_commit_validate(config, run_id):
    return combine_module_commit_validate(config, run_id)


def combine_m3_commit_validate(config, run_id):
    return combine_module_commit_validate(config, run_id)


def combine_module_commit_validate(config, run_id):
    m3 = config.checkpoint == COMBINE_M3_CHECKPOINT
    point = "P2.2" if m3 else "P2.1"
    log = config.logs / run_id
    cases = combine_m3_cases() if m3 else combine_m12_cases()
    summary = dict(run_id=run_id, checkpoint=config.checkpoint, mode=config.mode, status="FAIL",
                   implemented_through="M3" if m3 else "M2", scope="partial", cases=[], case_manifest=cases,
                   expected_cases=len(cases), base_cases=12 if m3 else 18, phases=[], gates={
                       gate: dict(status="NOT_RUN", expected_cases=sum(case["gate"] == gate for case in cases))
                       for gate in (("F", "P", "B", "regression", "connection") if m3 else ("F1", "F2", "P", "B", "regression", "connection", "edges"))},
                   errors=[], build_timeout_seconds=900, per_case_timeout_seconds=config.timeout,
                   coverage=("M1/M2/M3 single launch, DIRECT payload/Flag and module profiling; no delay, M4/M5 or yOut arithmetic" if m3 else "M0/M1/M2 stage/control/protection and 50us wait; no M3-M5 or yOut arithmetic"))
    original_environment = dict(os.environ)
    active_config = None
    stage = "manifest"
    print(f"{point} 共 {len(cases)} 个串行16P用例；首错停止，不自动重跑。", flush=True)
    for index, case in enumerate(cases, 1):
        print(f"[{index}/{len(cases)}] PLAN " + json.dumps(case, ensure_ascii=False), flush=True)
    try:
        owned = ("RUN_MODE", "LOOPS", "WARMUP", "BS", "H", "TOPK", "MOE_EXPERT_NUM", "DTYPE",
                 "QUANT_MODE", "DISPATCH_QUANT_MODE", "COMBINE_QUANT_MODE", "OPERATOR_MODE", "FP8_FORMAT",
                 "BLOCK_DIM", "CASE_ANALYSIS", "ROUTE_MODE", "SEED", "SERVERS", "BOUNDARY_PROBE",
                 "COMBINE_FIXTURE", "COMBINE_OBSERVATION")
        for key in owned:
            os.environ.pop("TILEXR_VALIDATION_" + key, None)
        os.environ.pop("TILEXR_DIRECT_HOSTS_JSON", None)
        defaults = combine_m12_defaults()
        if m3:
            defaults.update(loops=1, warmup=0)
        resume = os.environ.get("TILEXR_VALIDATION_RESUME_SUMMARY")
        retained = {}
        if resume and m3:
            raise Failure("RESULT_FAILED", f"{point} historical resume cannot qualify a new M3 implementation")
        if resume:
            stage = "resume-preflight"
            previous, retained, summary["cases"], summary["resume"] = load_combine_m12_resume(config, cases, resume, log)
            summary.update(expected_hosts=previous["expected_hosts"], rank_size=16)
            phase = dict(previous["phases"][0], reused=True, host_regression=str(log / "reused/off-host-summary.json"),
                         manifest=str(log / "builds/profile-off.manifest.json"))
            write_json(phase["manifest"], retained["profile-off"])
            summary["phases"].append(phase)
            for index, row in enumerate(summary["cases"], 1):
                print(f"[{index}/{len(cases)}] SKIP {row['name']} (reused PASS) 来源：{row['origin_summary']}", flush=True)
            for name, gate in summary["gates"].items():
                count = sum(row["gate"] == name for row in summary["cases"])
                if count:
                    gate.update(status="PASS", completed_cases=count)
            print(f"{point}续跑：跳过19项OFF及两次构建，仅执行原ON包Host检查/同步/4项breakdown；保留历史源码差异。", flush=True)
        for profiling in (False, True):
            if resume and not profiling:
                continue
            package = "profile-on" if profiling else "profile-off"
            os.environ.update({"TILEXR_VALIDATION_" + key.upper(): str(value) for key, value in defaults.items()})
            os.environ.update(TILEXR_VALIDATION_RESULT_ROOT=str(config.logs), TILEXR_VALIDATION_QUANT_MODE="0",
                              TILEXR_VALIDATION_RUN_MODE="breakdown" if profiling else "functional",
                              TILEXR_VALIDATION_BREAKDOWN=str(int(profiling)),
                              TILEXR_VALIDATION_TIMEOUT=str(config.timeout))
            active_config = Config("build")
            stage = ("reuse-" if resume else "build-") + package
            phase = dict(package=package, profiling=profiling, status="FAIL",
                         build_directory=str(validation_build_directory(active_config)),
                         install_directory=str(active_config.install))
            summary["phases"].append(phase)
            if resume:
                phase.update(build_reused=True, build_id=retained[package]["build_id"],
                             build_logs=previous["phases"][1]["build_logs"])
            else:
                try:
                    build(active_config)
                finally:
                    phase.update(build_id=getattr(active_config, "build_id", None),
                                 build_logs=str(config.logs / ("build-" + getattr(active_config, "build_id", "unknown"))))
            manifest = verify_build(active_config, check_run_mode=False)
            if resume and manifest != retained[package]:
                raise Failure("RESULT_FAILED", f"{point}续跑期间ON安装包发生变化")
            fingerprint = manifest["source"].get("content_sha256")
            if not isinstance(fingerprint, str) or not re.fullmatch(r"[0-9a-f]{64}", fingerprint):
                raise Failure("BUILD_FAILED", f"{point}缺少源码内容摘要")
            identity = dict(commit=manifest["source"]["commit"], content_sha256=fingerprint)
            if not resume and summary.get("source_identity", identity) != identity:
                raise Failure("BUILD_FAILED", f"{point} OFF/ON源码不一致，拒绝混合版本验收")
            if not resume:
                summary["source_identity"] = identity
            summary.update(commit=identity["commit"], source_provenance=manifest["source"]["provenance"])
            phase["manifest"] = str(log / "builds" / (package + ".manifest.json"))
            write_json(phase["manifest"], manifest)
            stage = "host-" + package
            host_id = run_id + "/" + stage
            phase["host_regression"] = str(config.logs / host_id / "summary.json")
            if host_validate(active_config, run_id=host_id):
                raise Failure("RESULT_FAILED", f"{point} Host回归失败；未启动本包设备用例")
            stage = "sync-" + package
            active_config = Config("run")
            hosts = active_config.hosts()
            summary.update(expected_hosts=hosts, rank_size=len(hosts) * 8)
            sync(active_config)
            for index, case in enumerate(cases, 1):
                if (case["run_mode"] == "breakdown") != profiling:
                    continue
                stage = case["name"]
                values = dict(defaults, **{key: value for key, value in case.items() if key not in ("name", "gate")})
                values["quant_mode"] = values["dispatch_quant_mode"]
                os.environ.update({"TILEXR_VALIDATION_" + key.upper(): str(value) for key, value in values.items()})
                os.environ["TILEXR_VALIDATION_RESULT_ROOT"] = str(log / "cases" / stage)
                active_config = Config("run")
                print(f"[{index}/{len(cases)}] START {stage} " + json.dumps(active_config.chain, ensure_ascii=False), flush=True)
                try:
                    code = run(active_config)
                except (Exception, KeyboardInterrupt) as error:
                    active_config.last_summary_path = active_config.logs / "failure.json"
                    write_json(active_config.last_summary_path, dict(status="FAIL", checkpoint=config.checkpoint,
                        build_id=manifest["build_id"], errors=failure_records(error, "RESULT_FAILED", stage=stage)))
                    code = 1
                report = read_json(active_config.last_summary_path)
                passed = (code == 0 and report.get("status") == "PASS" and report.get("rank_size") == 16
                          and report.get("completed_ranks") == 16 and report.get("build_id") == manifest["build_id"]
                          and report.get("checkpoint") == config.checkpoint)
                summary["cases"].append(dict(name=stage, gate=case["gate"], status="PASS" if passed else "FAIL",
                    build_id=manifest["build_id"], summary=str(active_config.last_summary_path),
                    configuration=dict(active_config.chain), completed_ranks=report.get("completed_ranks", 0)))
                gate = summary["gates"][case["gate"]]
                completed = sum(row["gate"] == case["gate"] and row["status"] == "PASS" for row in summary["cases"])
                gate.update(completed_cases=completed, status=("PASS" if completed == gate["expected_cases"] else "INCOMPLETE") if passed else "FAIL")
                print(f"[{index}/{len(cases)}] {'PASS' if passed else 'FAIL'} {stage} 结果：{active_config.last_summary_path}", flush=True)
                if not passed:
                    raise Failure("RESULT_FAILED", f"{point}用例失败、模块/构建不符或缺少完整16 rank：" + stage)
            phase["status"] = "PASS"
        if len(summary["cases"]) != len(cases) or any(gate["status"] != "PASS" for gate in summary["gates"].values()):
            raise Failure("RESULT_FAILED", f"{point}验收门槛缺失")
        summary["status"] = "PASS"
    except (Exception, KeyboardInterrupt) as error:
        summary["errors"] = failure_records(error, "BUILD_FAILED" if stage.startswith("build-") else "RESULT_FAILED", stage=stage)
        for failure in summary["errors"]:
            report_failure(failure)
    finally:
        os.environ.clear()
        os.environ.update(original_environment)
        config.cleanup_failed = (getattr(config, "cleanup_failed", False) or getattr(active_config, "cleanup_failed", False)
                                 or any(error["type"] == "CLEANUP_FAILED" for error in summary["errors"]))
        summary["cleanup"] = "failed" if config.cleanup_failed else "complete"
        write_json(log / "summary.json", summary)
    for name, gate in summary["gates"].items():
        print(f"{point} {name} {gate['status']} {gate.get('completed_cases', 0)}/{gate['expected_cases']}", flush=True)
    print(f"结果：{log / 'summary.json'}\n范围：{summary['coverage']}", flush=True)
    return 0 if summary["status"] == "PASS" else FAILURE_CODES[summary["errors"][0]["type"]]


def commit_validate(config):
    """Build and validate one delivered checkpoint; never advance the gate."""
    run_id = "commit-" + uuid.uuid4().hex
    if config.checkpoint in COMBINE_MEASUREMENT_CHECKPOINTS:
        code = (combine_m3_commit_validate(config, run_id) if config.checkpoint == COMBINE_M3_CHECKPOINT else
                combine_m12_commit_validate(config, run_id) if config.checkpoint == COMBINE_M12_CHECKPOINT
                else combine_profile_commit_validate(config, run_id))
        print(f"{'成功' if code == 0 else '失败'} COMMIT_VALIDATION_{'PASS' if code == 0 else 'FAIL'} "
              f"checkpoint={config.checkpoint}\n结果：{config.logs / run_id / 'summary.json'}", flush=True)
        return code
    try:
        build(config)
    except (Exception, KeyboardInterrupt) as error:
        errors = failure_records(error, "BUILD_FAILED", stage="build")
        config.cleanup_failed = any(record["type"] == "CLEANUP_FAILED" for record in errors)
        try:
            source = inventory(config.root)
        except Exception:
            source = {"commit": None, "provenance": "unavailable"}
        write_json(config.logs / run_id / "summary.json", {
            "run_id": run_id, "checkpoint": config.checkpoint, "mode": config.mode,
            "commit": source["commit"], "source_provenance": source["provenance"],
            "status": "FAIL", "tests": [], "errors": errors,
            "scope": "Combine 检查点构建失败；未执行本次验收测试。",
            "build_logs": str(config.logs / ("build-" + getattr(config, "build_id", "unknown"))),
            "build_timeout_seconds": 900,
            "validation_timeout_seconds": config.timeout,
            "per_test_timeout_seconds": 120,
        })
        for record in errors:
            report_failure(record)
        code = FAILURE_CODES[errors[0]["type"]]
    else:
        code = (combine_chain_commit_validate(config, run_id) if config.checkpoint in COMBINE_DEVICE_CHECKPOINTS
                else host_validate(config, run_id=run_id))
    print(f"{'成功' if code == 0 else '失败'} COMMIT_VALIDATION_{'PASS' if code == 0 else 'FAIL'} "
          f"checkpoint={config.checkpoint}\n结果：{config.logs / run_id / 'summary.json'}", flush=True)
    return code


def case_dry_run(config):
    """Resolve the normal case/rank contract locally without starting SSH or ACL."""
    manifest = verify_build(config, check_run_mode=False)
    mappings = rank_mapping(config.hosts())
    run_id = "dry-run-" + uuid.uuid4().hex
    result = dict(run_id=run_id, build_id=manifest["build_id"], checkpoint=config.checkpoint,
                  commit=manifest["source"]["commit"], mode="host", status="FAIL", ranks=[], errors=[],
                  scope="C3 local configuration/rank dry-run; no device or network operations",
                  chain_configuration=dict(config.chain), rank_size=len(mappings), completed_ranks=0)
    deadline = time.monotonic() + config.timeout
    try:
        env = dict(os.environ, TILEXR_VALIDATION_CHECKPOINT=config.checkpoint)
        for key, value in config.chain.items():
            env["TILEXR_VALIDATION_" + key.upper()] = str(value)
        env.pop("TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM", None)
        if config.chain["block_dim"] != "auto":
            env["TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM"] = config.chain["block_dim"]
        for mapping in mappings:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("EXIT_FAILED", "C3 dry-run 超过总期限", timeout=True)
            argv = [config.install / "bin" / COMBINE_DRY_RUN_PROGRAM, "--dry-run", run_id,
                    manifest["build_id"], manifest["source"]["commit"] or "unknown",
                    *[str(mapping[key]) for key in ("host", "host_index", "local_rank", "device", "rank", "rank_size")]]
            output = execute(argv, config.logs / run_id / f"rank-{mapping['rank']}.log",
                             timeout=remaining, cwd=config.root, env=env)
            result["ranks"].append(parse_rank_marker(output, COMBINE_DRY_RUN_MARKER))
        validate_rank_records(result["ranks"], mappings, run_id, manifest, config.chain)
        for record, mapping in zip(result["ranks"], mappings):
            if any(type(record.get(key)) is not type(value) for key, value in rank_record(mapping, run_id, manifest).items()):
                raise Failure("RESULT_FAILED", "C3 rank 身份字段类型不一致", rank=mapping["rank"])
            for key in ("operator_mode", "dispatch_quant_mode", "combine_quant_mode", "quant_mode",
                        "fp8_format", "run_mode", "loops", "warmup"):
                if type(record.get(key)) is not type(config.chain[key]) or record[key] != config.chain[key]:
                    raise Failure("RESULT_FAILED", "C3 配置回显不一致：" + key, rank=record["rank"])
            for key in ("device_allocated", "communicator_created", "kernel_launched", "network_used"):
                if record.get(key) is not False:
                    raise Failure("RESULT_FAILED", "C3 未确认无副作用：" + key, rank=record["rank"])
        result["status"] = "PASS"
    except (Exception, KeyboardInterrupt) as error:
        result["errors"] = failure_records(error, "RESULT_FAILED")
        for failure in result["errors"]:
            report_failure(failure)
    result["completed_ranks"] = len(result["ranks"]) if result["status"] == "PASS" else 0
    result["cleanup"] = "failed" if any(error["type"] == "CLEANUP_FAILED" for error in result["errors"]) else "complete"
    report = config.logs / run_id / "summary.json"
    write_json(report, result)
    run_summary.write_result(config.logs, result, report)
    return 0 if result["status"] == "PASS" else FAILURE_CODES[result["errors"][0]["type"]]


def node(config, run_id, build_id, execution_budget=None, device=None, host_index=None):
    log = config.logs / identifier(run_id)
    log.mkdir(parents=True, exist_ok=True)
    result = {"run_id": run_id, "build_id": build_id, "host": socket.gethostname(),
              "checkpoint": config.checkpoint, "mode": config.mode, "status": "FAIL", "tests": [], "errors": []}
    write_json(log / "pid.json", {"pid": os.getpid(), "start": pid_identity(os.getpid())})
    print(f"NODE_STARTED run_id={run_id}", flush=True)
    cancelled = threading.Event()

    def interrupted(signum, frame):
        if config.checkpoint in RANK_CHECKPOINTS:
            cancelled.set()
            return
        raise InterruptedError(f"节点收到停止信号 {signum}")

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    budget = min(120, config.timeout - 15)
    if execution_budget is not None:
        budget = min(budget, max(1, float(execution_budget)))
    deadline = time.monotonic() + budget
    current_test = None
    try:
        if (log / "cancelled").exists():
            raise Failure("EXIT_FAILED", "控制端已取消本次运行")
        manifest = verify_build(config)
        if manifest["build_id"] != build_id:
            raise Failure("RESULT_FAILED", "远端 build_id 与本次请求不一致")
        for test in node_tests(manifest):
            current_test = test["name"]
            if cancelled.is_set():
                raise InterruptedError("节点已收到取消信号")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("EXIT_FAILED", "节点测试超过执行期限", timeout=True)
            run_validation_test(config, test, log, remaining, device=device,
                                cancelled=cancelled if config.checkpoint in RANK_CHECKPOINTS else None)
            if current_test == REGISTRATION_TEST:
                result["device"] = int(device)
            result["tests"].append({"name": test["name"], "status": "PASS"})
        current_test = None
        if config.checkpoint in RANK_CHECKPOINTS:
            mappings = rank_mapping(config.hosts())
            index = int(host_index) if host_index is not None else -1
            if not 0 <= index < len(mappings) // 8:
                raise ValueError("多 rank 检查点缺少有效的 hostfile 节点序号")
            run_ranks(config, run_id, manifest, mappings[index * 8:(index + 1) * 8],
                              deadline, cancelled, result)
        if not result["errors"] and not cancelled.is_set():
            result["status"] = "PASS"
        elif not result["errors"]:
            raise InterruptedError("节点已收到取消信号")
    except Exception as error:
        kind = "EXIT_FAILED" if isinstance(error, InterruptedError) else "CONFIG_FAILED"
        result["errors"] = failure_records(error, kind, **({"test": current_test} if current_test else {}))
        if current_test:
            result["tests"].append({"name": current_test, "status": "FAIL",
                                    "failure_type": result["errors"][0]["type"]})
    finally:
        config.cleanup_failed = any(error["type"] == "CLEANUP_FAILED" for error in result["errors"])
        result["cleanup"] = "failed" if config.cleanup_failed else "complete"
        write_json(log / "result.json", result)
        if not config.cleanup_failed:
            (log / "pid.json").unlink(missing_ok=True)
    print("NODE_RESULT " + json.dumps(result, ensure_ascii=False), flush=True)
    return 0 if result["status"] == "PASS" else FAILURE_CODES[result["errors"][0]["type"]]


def cancel(config, run_id):
    log = config.logs / identifier(run_id)
    log.mkdir(parents=True, exist_ok=True)
    (log / "cancelled").touch()
    record = log / "pid.json"
    if not record.exists():
        return
    data = read_json(record)
    if data["start"] is None or pid_identity(data["pid"]) != data["start"]:
        raise ValueError("无法确认待清理进程身份，未发送信号")
    os.kill(data["pid"], signal.SIGTERM)
    deadline = time.monotonic() + 8
    while record.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    if record.exists():
        raise RuntimeError("节点清理未确认，保留日志和 PID 记录")


def validate_result(result, run_id, manifest, expected_ranks=None, chain_config=None):
    fields = ("run_id", "build_id", "checkpoint", "mode", "status", "cleanup", "tests")
    if (not isinstance(result, dict) or any(field not in result for field in fields)
            or not isinstance(result["tests"], list)
            or any(not isinstance(test, dict) or not isinstance(test.get("name"), str)
                   or test.get("status") not in ("PASS", "FAIL") for test in result["tests"])
            or result["status"] not in ("PASS", "FAIL")):
        raise Failure("LOG_PARSE_FAILED", "节点结果缺少字段或格式错误")
    expected = {test["name"] for test in node_tests(manifest)}
    actual = [test["name"] for test in result["tests"]]
    if (result["run_id"] != run_id or result["build_id"] != manifest["build_id"]
            or result["checkpoint"] != manifest["checkpoint"] or result["mode"] != manifest["mode"]):
        raise Failure("RESULT_FAILED", "节点结果不属于本次运行或构建")
    errors = result.get("errors", [])
    if (not isinstance(errors, list) or any(not isinstance(error, dict)
            or error.get("type") not in FAILURE_LABELS or not isinstance(error.get("message"), str)
            for error in errors)):
        raise Failure("LOG_PARSE_FAILED", "节点错误记录格式错误")
    if errors:
        first = errors[0]
        raise Failure(first["type"], first["message"],
                      **{key: value for key, value in first.items() if key not in ("type", "message")},
                      additional_failures=errors[1:])
    if result["cleanup"] != "complete":
        raise Failure("CLEANUP_FAILED", "节点未确认完成进程清理")
    if (result["status"] != "PASS" or len(actual) != len(expected) or set(actual) != expected
            or any(test["status"] != "PASS" for test in result["tests"])):
        raise Failure("RESULT_FAILED", "节点测试结果失败、缺失或重复")
    if manifest["checkpoint"] in RANK_CHECKPOINTS:
        if expected_ranks is None:
            raise Failure("RESULT_FAILED", "缺少预期 rank 映射")
        validate_rank_records(result.get("ranks"), expected_ranks, run_id, manifest, chain_config,
                              allow_invalid_output=True)


def result_errors(result, run_id, manifest, expected_ranks=None, chain_config=None):
    try:
        validate_result(result, run_id, manifest, expected_ranks, chain_config)
    except Failure as error:
        return failure_records(error, "RESULT_FAILED")
    return []


def launch_outcome(log, execution_error, run_id, manifest, expected_ranks=None, chain_config=None):
    """优先采用节点记录的失败边界，SSH 包装进程的非零退出不覆盖测试原因。"""
    raw = b""
    execution = failure_records(execution_error, "EXIT_FAILED") if execution_error else []
    try:
        raw = log.read_bytes()
        output = raw.decode("utf-8")
        completions = [line[len("NODE_RESULT "):] for line in output.splitlines() if line.startswith("NODE_RESULT ")]
        if completions:
            if len(completions) != 1:
                raise ValueError("节点完成记录重复")
            result = json.loads(completions[0])
            errors = result_errors(result, run_id, manifest, expected_ranks, chain_config)
            if errors:
                if execution and execution[0].get("returncode") != FAILURE_CODES[errors[0]["type"]]:
                    return result, [*execution, *errors]
                errors.extend(error for error in execution if error["type"] == "CLEANUP_FAILED")
                return result, errors
            return result, execution
        if not execution:
            raise ValueError("进程退出成功但缺少 NODE_RESULT 完成记录")
    except (OSError, UnicodeError, ValueError) as error:
        if not any(record["type"] == "LOG_PARSE_FAILED" for record in execution):
            execution.append(Failure("LOG_PARSE_FAILED", str(error), log=str(log)).record())
    if execution[0]["type"] == "EXIT_FAILED" and not any(line.startswith(b"NODE_STARTED ") for line in raw.splitlines()):
        execution[0]["type"] = "LAUNCH_FAILED"
    return None, execution


def rebase_node_breakdown_reports(result, node_log, host, *, local_records=False):
    """Keep failed-capture source evidence while relocating only collected reports."""
    if not isinstance(result, dict):
        return
    def relocate_operators(record):
        if not isinstance(record, dict) or type(record.get("rank")) is not int:
            return
        for operator, evidence in record.get("operator_measurements", {}).items():
            if operator in ("dispatch", "combine") and isinstance(evidence, dict) and isinstance(evidence.get("breakdown"), dict):
                evidence["breakdown"] = operator_measure.relocate_operator_breakdown_report(
                    evidence["breakdown"], node_log, record["rank"], host, operator=operator, local_records=local_records)

    for record in result.get("ranks", []):
        relocate_operators(record)
        if (isinstance(record, dict) and type(record.get("rank")) is int
                and isinstance(record.get("breakdown"), dict)):
            record["breakdown"] = measurements.relocate_breakdown_report(
                record["breakdown"], node_log, record["rank"], host, local_records=local_records)
    for item in [*result.get("errors", []), *result.get("profiling_failures", [])]:
        if (not isinstance(item, dict) or item.get("communication_completed") is not True
                or type(item.get("rank")) is not int):
            continue
        item["source_host"] = host
        item["source_records_path"] = item.get("source_records_path", item.get("records_path"))
        item["records_collected"] = local_records
        item["report_dir"] = str(Path(node_log) / f"profile-rank-{item['rank']}")
        communication = item.get("communication")
        relocate_operators(communication)
        if isinstance(communication, dict) and isinstance(communication.get("breakdown"), dict):
            communication["breakdown"] = measurements.relocate_breakdown_report(
                communication["breakdown"], node_log, item["rank"], host, local_records=local_records)


def node_profile_records(nodes, run_id, manifest, mappings, chain_config):
    """Only current-run, current-build rank records may enter the report index."""
    records = []
    expected = {mapping["rank"]: mapping for mapping in mappings}
    for node in nodes:
        result = node.get("reported_result")
        if (not isinstance(result, dict) or result.get("run_id") != run_id
                or any(result.get(key) != manifest[key] for key in ("build_id", "checkpoint", "mode"))):
            continue
        rows = list(result.get("ranks", []))
        rows.extend(item["communication"] for item in result.get("profiling_failures", [])
                    if isinstance(item, dict) and isinstance(item.get("communication"), dict))
        for row in rows:
            if not isinstance(row, dict) or type(row.get("rank")) is not int:
                continue
            mapping = expected.get(row["rank"])
            if mapping is None or mapping["host"] != node["host"]:
                continue
            try:
                validate_rank_records([row], [mapping], run_id, manifest, chain_config,
                                      check_measurements=False, allow_invalid_output=True)
            except (Failure, ValueError, TypeError, KeyError):
                continue
            records.append(row)
    return records


def run(config):
    deadline = time.monotonic() + config.timeout
    execution_deadline = min(deadline - 20, time.monotonic() + 120)
    hosts = config.hosts()
    mappings = rank_mapping(hosts) if config.checkpoint in RANK_CHECKPOINTS else None

    def expected_ranks(index):
        return mappings[index * 8:(index + 1) * 8] if mappings is not None else None
    check_coordinator(config, hosts)
    run_id = uuid.uuid4().hex
    log = config.logs / run_id
    manifest = verify_build(config)

    # Verify actual installations on this run's subset before launching any rank.
    # A sync receipt is provenance, not a signature of the requested case.
    def verify_selected(host):
        try:
            execute(remote_argv(config, host["host"], "verify", manifest["build_id"]),
                    log / ("verify-" + host["host"] + ".log"), timeout=60, stdin=REMOTE_SCRIPT,
                    start_failure="CONFIG_FAILED", exit_failure="CONFIG_FAILED")
        except Exception as error:
            raise Failure("CONFIG_FAILED", f"节点 {host['host']} 产物检查失败，请检查或同步该节点：{error}",
                          host=host["host"]) from error
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        for future in concurrent.futures.as_completed([pool.submit(verify_selected, host) for host in hosts[1:]]):
            future.result()
    # Forward a resolved subset even to local child processes.
    os.environ["TILEXR_DIRECT_HOSTS_JSON"] = json.dumps(hosts, separators=(",", ":"))
    print("RUN_CONFIG " + json.dumps({"servers": len(hosts), "rank_size": len(hosts) * 8,
          "hosts": [host["host"] for host in hosts], "devices_per_server": list(range(8)),
          "build_id": manifest["build_id"], "local_experts": config.chain["moe_expert_num"] // (8 * len(hosts)) if config.chain else None,
          "case": config.chain}, ensure_ascii=False), flush=True)
    print(f"开始测试：hosts={len(hosts)} checkpoint={config.checkpoint} mode={config.mode}\n日志：{log}", flush=True)

    def launch(index, host):
        budget = max(1, execution_deadline - time.monotonic())
        arguments = ["node", run_id, manifest["build_id"], str(budget)]
        if config.checkpoint == EMPTY_KERNEL_CHECKPOINT:
            arguments.append(str(host["devices"][0]))
        elif config.checkpoint in RANK_CHECKPOINTS:
            arguments.extend(["", str(index)])
        if index == 0:
            argv = [sys.executable, Path(__file__), *arguments]
            payload = None
        else:
            argv = remote_argv(config, host["host"], *arguments)
            payload = REMOTE_SCRIPT
        launch_log = log / f"launch-{index}.log"
        execution_error = None
        try:
            execute(argv, launch_log, timeout=max(1, deadline - time.monotonic() - 12), stdin=payload)
        except Failure as error:
            execution_error = error
        return launch_outcome(launch_log, execution_error, run_id, manifest, expected_ranks(index), config.chain)

    errors = []
    cleanup_errors = []
    primary_error = None
    launched = {}
    launch_results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        futures = {pool.submit(launch, index, host): index for index, host in enumerate(hosts)}
        cancelled = False
        pending = set(futures)
        while pending:
            # SSH 建连时间也计入统一执行期限；不能等远端的相对 watchdog 到期才取消。
            remaining = None if cancelled else max(0, execution_deadline - time.monotonic())
            done, pending = concurrent.futures.wait(pending, timeout=remaining,
                                                    return_when=concurrent.futures.FIRST_COMPLETED)
            for future in done:
                index = futures[future]
                try:
                    launch_results[index], launched[index] = future.result()
                except Exception as error:
                    launch_results[index] = None
                    launched[index] = [failure_record(error, "LAUNCH_FAILED")]
                if launched[index] and primary_error is None:
                    primary_error = dict(launched[index][0], host=hosts[index]["host"])
            if not done:
                primary_error = Failure("EXIT_FAILED", "集群达到统一执行期限", timeout=True,
                                        rank="unknown", stage="execution").record()
                errors.append(primary_error)
            if primary_error is not None and not cancelled:
                cancelled = True
                # 独立连接通知仍在运行的节点；每个节点自身也有执行期限。
                def stop(index, host):
                    try:
                        if index == 0:
                            cancel(config, run_id)
                        else:
                            execute(remote_argv(config, host["host"], "cancel", run_id),
                                    log / f"cancel-{index}.log", timeout=10, stdin=REMOTE_SCRIPT,
                                    start_failure="CLEANUP_FAILED", exit_failure="CLEANUP_FAILED")
                    except Exception as failure:
                        return failure_record(failure, "CLEANUP_FAILED", host=host["host"], stage="cleanup")
                with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as cleanup:
                    for failure in cleanup.map(lambda item: stop(*item), enumerate(hosts)):
                        if failure:
                            cleanup_errors.append(failure)

    def collect(index, host):
        node_log = log if index == 0 else log / "nodes" / str(index)
        failures = list(launched[index])
        result = launch_results[index]
        try:
            if index:
                node_log.mkdir(parents=True, exist_ok=True)
                excludes = (["--exclude=*.records.v1", "--exclude=*.records.v1.tmp"]
                            if config.chain and config.chain["run_mode"] == "breakdown" else [])
                execute(["rsync", "-a", "--protect-args", "--timeout=5", *excludes, "-e", shlex.join(["ssh", *SSH_OPTIONS]),
                         config.target(host["host"]) + ":" + str(log) + "/", str(node_log) + "/"],
                        log / f"collect-{index}.log", timeout=max(1, deadline - time.monotonic()),
                        start_failure="LOG_COLLECT_FAILED", exit_failure="LOG_COLLECT_FAILED")
            result = read_json(node_log / "result.json")
            failures.extend(result_errors(result, run_id, manifest, expected_ranks(index), config.chain))
            # Launches have all finished. Fold output mismatches into the final
            # result now, after the cancellation decision, while retaining traces.
            if not failures and config.checkpoint in (DISPATCH_CHECKPOINT, COMBINE_M3_CHECKPOINT):
                failures.extend(Failure("RESULT_FAILED", "输出校验失败：" + rank["validation_error"],
                                        rank=rank["rank"], stage=rank["validation_stage"],
                                        validation_status="FAIL", communication_completed=True).record()
                                for rank in result["ranks"] if rank.get("validation_status") == "FAIL")
            if config.chain and config.chain["run_mode"] == "breakdown":
                rebase_node_breakdown_reports(result, node_log, host["host"], local_records=index == 0)
                rebase_node_breakdown_reports({"errors": failures}, node_log, host["host"], local_records=index == 0)
        except FileNotFoundError as error:
            # 拉起/异常退出已说明为何没有完成记录，避免追加一个伪解析失败。
            if not failures:
                failures.append(Failure("LOG_PARSE_FAILED", str(error), log=str(node_log / "result.json")).record())
        except (OSError, UnicodeError, ValueError) as error:
            failures.append(Failure("LOG_PARSE_FAILED", str(error), log=str(node_log / "result.json")).record())
        except Failure as error:
            failures.extend(failure_records(error, "LOG_COLLECT_FAILED"))
        unique = {(item["type"], item["message"], item.get("test"), item.get("rank")): item for item in failures}
        failures = [dict(item, host=host["host"]) for item in unique.values()]
        tests = result.get("tests", []) if isinstance(result, dict) else []
        if not isinstance(tests, list):
            tests = []
        return {"host": host["host"], "status": "FAIL" if failures else "PASS",
                "tests": sum(test.get("status") == "PASS" for test in tests if isinstance(test, dict)),
                "reported_result": result,
                "errors": failures}

    nodes = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        futures = {pool.submit(collect, index, host): index for index, host in enumerate(hosts)}
        for future in concurrent.futures.as_completed(futures):
            index = futures[future]
            try:
                collected = future.result()
                nodes.append(collected)
                errors.extend(collected["errors"])
            except Exception as error:
                errors.append(failure_record(error, "LOG_COLLECT_FAILED", host=hosts[index]["host"]))
    if time.monotonic() > deadline:
        errors.append(Failure("LOG_COLLECT_FAILED", "结果汇总超过总期限", timeout=True).record())
    errors.extend(cleanup_errors)
    if primary_error is None and errors:
        primary_error = errors[0]
    config.cleanup_failed = any(error["type"] == "CLEANUP_FAILED" for error in errors)
    scope = "ABI/Host"
    if config.checkpoint == EMPTY_KERNEL_CHECKPOINT:
        scope = "A5 空 kernel 安装与设备注册"
    elif config.checkpoint == DRY_RUN_CHECKPOINT:
        scope = f"{len(mappings)}P rank/device 映射与进程退出；不创建设备上下文或 communicator"
    if config.checkpoint == MEMORY_BOUNDARY_CHECKPOINT:
        scope = (f"{len(mappings)}P 每 Rank 4 GiB MemoryDomain 分配和 peer 映射；"
                 "本卡末尾 32B DataCopy 写入/读回、Host 比较及前邻 32B 零值检查；未验证跨卡数据传输或去重算法。")
    elif config.checkpoint == COMBINE_BOUNDARY_CHECKPOINT:
        scope = (f"Combine C7：{len(mappings)}P 双窗8点独立边界探针，避开runtime；不提交业务算子。"
                 if config.chain["boundary_probe"] else f"Combine C7：{len(mappings)}P {config.chain['operator_mode']} "
                 "连续预热/正式调用回归，保留真实Dispatch输出/五表/Flag校验；不验Combine业务数据。")
    elif config.checkpoint == COMBINE_M3_CHECKPOINT:
        scope = (f"{len(mappings)}P M3 single launch with no warmup: M1/M2 plus DIRECT payload/Flag; "
                 "functional/performance/module breakdown; scope=partial implemented_through=M3; no M4/M5 or yOut arithmetic.")
    elif config.checkpoint == COMBINE_M12_CHECKPOINT:
        scope = (f"Combine P2.1：{len(mappings)}P {config.chain['operator_mode']} {config.chain['run_mode']}，"
                 "M1原始stage数据/M2控制清理/保护区及各自magic选窗；64核保留50us；"
                 "scope=partial implemented_through=M2；不验M3-M5返回归约或yOut业务数值。")
    elif config.checkpoint == COMBINE_PROFILE_CHECKPOINT:
        scope = (f"Combine C6：{len(mappings)}P {config.chain['operator_mode']} {config.chain['run_mode']}，"
                 "连续预热/正式调用、各算子独立采集和trace；空Combine等待50us，算法字节0/带宽N/A；不验Combine通信。")
    elif config.checkpoint == COMBINE_MULTI_CHECKPOINT:
        scope = (f"Combine C5：{len(mappings)}P {config.chain['operator_mode']}，{config.chain['warmup']}轮预热＋"
                 f"{config.chain['loops']}轮正式调用，同stream连续提交、批末同步、各算子magic/选窗及生命周期；"
                 "Dispatch检查最终输出与自身窗口；空Combine等待50us，不验Combine通信或yOut业务数值。")
    elif config.checkpoint == COMBINE_CHAIN_CHECKPOINT and config.chain["operator_mode"] == "dispatch":
        scope = f"Combine C4 安装包的 {len(mappings)}P Dispatch 独跑回归：完整单轮Dispatch与五表校验；未提交Combine。"
    elif config.checkpoint == COMBINE_CHAIN_CHECKPOINT:
        scope = (f"Combine C4：{len(mappings)}P {config.chain['operator_mode']} 单轮真实调用、同步、"
                 "输入/yOut保持、五表/count连接及各算子magic选窗；空Combine等待50us，不验Combine通信或yOut业务数值。")
    elif config.checkpoint in CHAIN_CHECKPOINTS:
        scope = (f"{len(mappings)}P 每 rank 8 GiB communicator（两个 4 GiB window）、"
                  f"{config.chain['warmup']} 轮预热＋{config.chain['loops']} 轮测量（同域连续提交）、"
                  "最后一轮 cnt/cumsum、DIRECT/第一跳/终端完整 route 覆盖、源端输出、"
                  "最终 hidden/scales/五表、全局路由参考及各 Rank 成员地址本地匹配和轮末 Flag 清理校验；不包含 Combine。")
    else:
        scope += "；未执行 NPU launch 或去重通信。"
    report = {"run_id": run_id, "commit": manifest["source"]["commit"], "build_id": manifest["build_id"],
              "source_provenance": manifest["source"]["provenance"],
              "checkpoint": config.checkpoint, "mode": config.mode, "status": "FAIL" if errors else "PASS",
              "expected_hosts": hosts, "server_count": len(hosts), "nodes": nodes, "primary_error": primary_error, "errors": errors,
              "scope": scope}
    rank_summary = ""
    if mappings is not None:
        completed = len(node_profile_records(nodes, run_id, manifest, mappings, config.chain)) if (
            config.checkpoint in (DISPATCH_CHECKPOINT, COMBINE_M3_CHECKPOINT)) else sum(
            len(node["reported_result"].get("ranks", [])) for node in nodes if node["status"] == "PASS")
        report.update(rank_size=len(mappings), completed_ranks=completed, rank_mapping=mappings)
        if config.chain is not None:
            report["chain_configuration"] = dict(config.chain)
            if (config.checkpoint in COMBINE_DEVICE_CHECKPOINTS and not config.chain.get("boundary_probe")
                    and config.chain["operator_mode"] != "combine" and not errors):
                try:
                    dispatch_records = [rank["dispatch"] if config.chain["operator_mode"] == "both" else rank
                                        for node in nodes for rank in node["reported_result"]["ranks"]]
                    validate_dispatch_tables(dispatch_records, dict(config.chain, rank_size=len(mappings)))
                    report["dispatch_five_tables_validated"] = True
                except (ValueError, TypeError, KeyError) as error:
                    failure = Failure("RESULT_FAILED", f"Dispatch 跨rank五表校验失败：{error}").record()
                    errors.append(failure)
                    primary_error = failure
                    report.update(status="FAIL", primary_error=failure)
            if config.checkpoint in COMBINE_MEASUREMENT_CHECKPOINTS and config.chain["run_mode"] != "functional":
                records = node_profile_records(nodes, run_id, manifest, mappings, config.chain)
                full_config = dict(config.chain, rank_size=len(mappings))
                kernel_shas = operator_kernel_shas(manifest, config.chain)
                try:
                    report["operator_measurements"] = operator_measure.summarize_operator_measurements(
                        records, full_config, run_id, kernel_shas)
                except (ValueError, TypeError, KeyError, OSError) as error:
                    errors.append(Failure("RESULT_FAILED", f"C6算子测量汇总失败：{error}").record())
                if config.chain["run_mode"] == "breakdown":
                    try:
                        report["operator_traces"] = operator_measure.collect_operator_breakdown_reports(
                            records, full_config, log / "traces", run_id, kernel_shas)
                        for operator, traces in report["operator_traces"].items():
                            for error in traces["errors"]:
                                errors.append(Failure("LOG_COLLECT_FAILED", f"{operator} profiling report: " + error["message"],
                                    **({"rank": error["rank"]} if "rank" in error else {})).record())
                            if traces.get("status") != "complete":
                                report.get("operator_measurements", {}).pop(operator, None)
                    except (ValueError, TypeError, KeyError, OSError) as error:
                        errors.append(Failure("LOG_COLLECT_FAILED", f"C6独立trace汇总失败：{error}").record())
                if errors:
                    primary_error = primary_error or errors[0]
                    report.update(status="FAIL", primary_error=primary_error)
            if config.checkpoint == DISPATCH_CHECKPOINT:
                records = node_profile_records(nodes, run_id, manifest, mappings, config.chain)
                full_config = dict(config.chain, rank_size=len(mappings))
                kernel_sha = manifest["artifacts"]["lib/tilexr/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.o"]
                if not errors:
                    try:
                        validate_dispatch_tables(records, full_config)
                        report["five_tables_validated"] = True
                    except (ValueError, TypeError, KeyError) as error:
                        failure = Failure("RESULT_FAILED", f"Dispatch 跨 rank 校验失败：{error}",
                                          validation_status="FAIL", communication_completed=True).record()
                        errors.append(failure)
                        primary_error = failure
                        report.update(status="FAIL", primary_error=failure)
                # A failed correctness verdict must not discard complete timing.
                # Missing/invalid captures remain failures, never partial-rank aggregates.
                try:
                    aggregate = measurements.summarize_measurements(records, full_config, kernel_sha,
                        output_valid=report.get("five_tables_validated", False))
                    report["chain_configuration"]["server_dedup_enabled"] = aggregate["configuration"]["server_dedup_enabled"]
                    if config.chain["run_mode"] != "functional":
                        report["measurements"] = aggregate
                except (ValueError, TypeError, KeyError, OSError) as error:
                    if not errors:
                        failure = Failure("RESULT_FAILED", f"Dispatch 测量汇总失败：{error}").record()
                        errors.append(failure)
                        primary_error = failure
                        report.update(status="FAIL", primary_error=failure)
                    else:
                        report["measurement_error"] = str(error)
            if config.checkpoint == DISPATCH_CHECKPOINT and config.chain["run_mode"] == "breakdown":
                try:
                    kernel_sha = manifest["artifacts"]["lib/tilexr/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.o"]
                    report["traces"] = measurements.collect_breakdown_reports(
                        node_profile_records(nodes, run_id, manifest, mappings, config.chain),
                        dict(config.chain, rank_size=len(mappings)), log / "traces", run_id, kernel_sha)
                    for item in report["traces"]["errors"]:
                        errors.append(Failure("LOG_COLLECT_FAILED", "profiling report: " + item["message"],
                                              **({"rank": item["rank"]} if "rank" in item else {})).record())
                except (ValueError, TypeError, KeyError, OSError) as error:
                    errors.append(Failure("LOG_COLLECT_FAILED", f"profiling report index failed: {error}").record())
                if report.get("traces", {}).get("status") != "complete":
                    report.pop("measurements", None)
                if errors:
                    primary_error = primary_error or errors[0]
                    report.update(status="FAIL", primary_error=primary_error)
        rank_summary = f"ranks={completed}/{len(mappings)} "
    # All launch futures and log collection have completed above. Read the
    # retained real inputs only here, never while another rank is on the NPU.
    if config.chain and config.chain.get("case_analysis"):
        try:
            if config.cleanup_failed:
                raise ValueError("NPU process cleanup is unconfirmed; case analysis skipped")
            from case_analysis import analyze_case
            report["case_analysis"] = analyze_case(log, dict(config.chain, rank_size=len(mappings)),
                node_profile_records(nodes, run_id, manifest, mappings, config.chain))
        except (ValueError, TypeError, KeyError, OSError) as error:
            failure = Failure("RESULT_FAILED", f"case 分析失败：{error}").record()
            errors.append(failure)
            primary_error = primary_error or failure
            report.update(status="FAIL", primary_error=primary_error,
                          case_analysis=dict(status="failed", error=str(error)))
    config.last_summary_path = log / "summary.json"
    write_json(config.last_summary_path, report)
    run_summary.write_result(config.logs, report, log / "summary.json")
    print("\n================================", flush=True)
    print(measurements.render_case_description(report), flush=True)
    if "measurements" in report:
        print(measurements.render_measurements(report["measurements"]), flush=True)
    if "operator_measurements" in report:
        print(operator_measure.render_operator_measurements(report["operator_measurements"]), flush=True)
    for operator, traces in report.get("operator_traces", {}).items():
        print(f"{operator} Chrome tracing JSON 目录: {traces['trace_dir']}", flush=True)
    if "traces" in report:
        print("Chrome tracing JSON 目录（每 rank 一份）: " + report["traces"]["trace_dir"], flush=True)
        for role, path in report["traces"]["selected"].items():
            print(f"{role}: {path}", flush=True)
    print(f"VALIDATION_{report['status']} checkpoint={config.checkpoint} mode={config.mode} "
          f"hosts={sum(node['status'] == 'PASS' for node in nodes)}/{len(hosts)} "
          f"tests_per_host={len(node_tests(manifest))} {rank_summary}commit={report['commit'] or 'unknown'} (unverified)", flush=True)
    print("================================\n", flush=True)
    for error in errors:
        report_failure(error)
    print(f"结果：{log / 'summary.json'}\n范围：{scope}", flush=True)
    return FAILURE_CODES[primary_error["type"]] if errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("env", "build", "sync", "run", "host-validate", "commit-validate", "case-dry-run", "node", "rank", "verify", "cancel"))
    parser.add_argument("arguments", nargs="*")
    parser.add_argument("--debug", action="store_true", help="仅用于 sync：逐主机 SSH 和 rsync dry-run 检查")
    args = parser.parse_args()
    if args.debug and args.action != "sync":
        parser.error("--debug 仅支持 sync")
    try:
        if sys.version_info < (3, 9):
            raise ValueError("七步验证工具要求 Python 3.9 或更新版本")
        if args.action == "cancel":
            # 不解析 hostfile/CANN/运行配置；旧任务清理不能依赖已经改坏的环境。
            class CancelConfig:
                logs = Path(os.environ.get("TILEXR_VALIDATION_RESULT_ROOT", Path(__file__).resolve().parents[2] / "run/validation"))
            cancel(CancelConfig(), *args.arguments)
            return 0
        config = Config(args.action)
        if args.action == "env":
            hosts = config.hosts()
            print(f"ENV_READY checkpoint={config.checkpoint} mode={config.mode} "
                  f"hosts={len(hosts)} hostfile={config.hostfile} root={config.root}")
        elif args.action == "build":
            with operation_lock(config, ".operation.lock"):
                build(config)
        elif args.action == "sync":
            if args.arguments:
                raise ValueError("sync 仅接受 --debug 选项")
            with operation_lock(config, ".operation.lock"):
                if args.debug:
                    return sync_debug(config)
                sync(config)
        elif args.action == "case-dry-run":
            if args.arguments:
                raise ValueError("case-dry-run 不接受额外参数")
            with operation_lock(config, ".operation.lock"):
                return case_dry_run(config)
        elif args.action == "run":
            with operation_lock(config, ".operation.lock"):
                return run(config)
        elif args.action == "host-validate":
            if args.arguments:
                raise ValueError("host-validate 不接受额外参数")
            with operation_lock(config, ".operation.lock"):
                return host_validate(config)
        elif args.action == "commit-validate":
            if args.arguments:
                raise ValueError("commit-validate 不接受额外参数")
            with operation_lock(config, ".operation.lock"):
                return commit_validate(config)
        elif args.action == "node":
            with operation_lock(config, "node.lock"):
                return node(config, *args.arguments)
        elif args.action == "rank":
            return dry_run_rank(config, *args.arguments)
        elif args.action == "verify":
            manifest = verify_build(config, check_run_mode=False)
            if manifest["build_id"] != args.arguments[0]:
                raise ValueError("同步后的 build_id 不一致")
            print("VERIFY_PASS")
        return 0
    except Exception as error:
        kind = {"build": "BUILD_FAILED", "sync": "SYNC_FAILED", "cancel": "CLEANUP_FAILED",
                "node": "LAUNCH_FAILED"}.get(args.action, "CONFIG_FAILED")
        records = failure_records(error, kind, stage=args.action)
        if args.action in ("run", "case-dry-run"):
            directory = Path(os.environ.get("TILEXR_VALIDATION_RESULT_ROOT", "run/validation"))
            report = dict(status="FAIL", primary_error=records[0], errors=records)
            write_json(directory / "failure.json", report)
            run_summary.write_result(directory, report, directory / "failure.json")
        for record in records:
            report_failure(record)
        return FAILURE_CODES[records[0]["type"]]


if __name__ == "__main__":
    sys.exit(main())
