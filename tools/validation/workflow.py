#!/usr/bin/env python3
"""EP 构建、分发、功能与性能验证。"""

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


COMBINE_DRY_RUN_TEST = "test_tilexr_ep_server_dedup_combine_dry_run"
COMBINE_DRY_RUN_PROGRAM = "test_tilexr_ep_server_dedup_chain_dry_run"
COMBINE_DRY_RUN_MARKER = "EP_SERVER_DEDUP_COMBINE_DRYRUN_PASS "
DISPATCH_CONSUMER_TEST = "test_tilexr_ep_server_dedup_dispatch_consumer"
COMBINE_CONSUMER_TEST = "test_tilexr_ep_server_dedup_combine_consumer"
COMBINE_CONFIG_TEST = "test_tilexr_ep_server_dedup_combine_config"
COMBINE_PROFILE_KERNEL_TEST = "test_tilexr_ep_server_dedup_combine_profile_kernel"
COMBINE_PROFILE_REGIONS_TEST = "test_tilexr_ep_server_dedup_combine_profile_regions"
COMBINE_ABI_HOST_TESTS = {
    "test_tilexr_ep_server_dedup_combine_" + name
    for name in ("public_abi", "layout", "host_validation")
}
DISPATCH_ABI_TESTS = {"test_tilexr_ep_server_dedup_" + name
                      for name in ("protocol", "layout", "host_validation", "public_abi")}
COMBINE_TESTS = COMBINE_ABI_HOST_TESTS | DISPATCH_ABI_TESTS | {
    "test_tilexr_ep_layout", "test_tilexr_ep_expert_ids",
    COMBINE_DRY_RUN_TEST, COMBINE_CONSUMER_TEST, DISPATCH_CONSUMER_TEST, COMBINE_CONFIG_TEST,
    COMBINE_PROFILE_KERNEL_TEST, COMBINE_PROFILE_REGIONS_TEST,
    "test_tilexr_ep_kernel_launch", "test_tilexr_ep_kernel_launch_profile",
    "test_tilexr_ep_server_dedup_combine_m5",
}
CONSUMER_TESTS = {COMBINE_DRY_RUN_TEST, COMBINE_CONSUMER_TEST, DISPATCH_CONSUMER_TEST}
COMBINE_INVALID_MARKER = "EP_SERVER_DEDUP_COMBINE_CHAIN_INVALID "
COMBINE_CHAIN_MARKER = "EP_SERVER_DEDUP_COMBINE_CHAIN_PASS "
DISPATCH_MARKER = "EP_SERVER_DEDUP_DISPATCH_PASS "
DISPATCH_INVALID_MARKER = "EP_SERVER_DEDUP_DISPATCH_INVALID "
DISPATCH_ABI = {"launch_bytes": 120, "tiling_bytes": 32, "runtime_args_bytes": 120,
                "layout": "r13-count-gm512", "kernel_scope": "dispatch"}
SERVER_DEDUP_PEER_BYTES = 8 * 1024 ** 3
MEMORY_BOUNDARY_BYTES = 4 * 1024 ** 3
CHAIN_PROGRAM = "test_tilexr_ep_server_dedup_chain"
CHAIN_MARKER = "EP_SERVER_DEDUP_CHAIN_PASS "
OPERATOR_BREAKDOWN_MARKER = "EP_SERVER_DEDUP_OPERATOR_BREAKDOWN_PASS "


def combine_contract(send_core_num=32):
    return dict(launch_bytes=88, tiling_bytes=16, runtime_args_bytes=88,
                block_dim=64, kernel_scope="combine-m5", implemented_through="M5",
                control_bytes=96 * 1024, send_core_num=send_core_num, delay_us=0, scope="full")


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


class PhaseTimer:
    """Disjoint wall-clock intervals within one process; never device timings."""
    def __init__(self, phase):
        self.started = self.previous = time.perf_counter()
        self.phase = phase
        self.seconds = {}

    def next(self, phase):
        now = time.perf_counter()
        self.seconds[self.phase] = self.seconds.get(self.phase, 0.0) + now - self.previous
        self.previous, self.phase = now, phase

    def snapshot(self):
        now = time.perf_counter()
        seconds = dict(self.seconds)
        seconds[self.phase] = seconds.get(self.phase, 0.0) + now - self.previous
        return dict(unit="seconds", total=now - self.started, stages=seconds)


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
        self.is_run = action in ("run", "node", "case-dry-run")
        self.root = Path(os.environ.get("TILEXR_VALIDATION_ROOT", Path(__file__).resolve().parents[2])).resolve()
        self.mode = os.environ.get("TILEXR_VALIDATION_BUILD_MODE", "production")
        self.checkpoint = None  # Reserved manifest identity; no stage selection.
        local_only = action in ("build", "verify", "host-validate")
        self.hostfile = (self.root / "hostfile" if local_only else
                         Path(os.environ.get("TILEXR_VALIDATION_HOSTFILE", self.root / "hostfile")))
        self.user = "" if local_only else os.environ.get("TILEXR_VALIDATION_SSH_USER", "")
        self.jobs = int(os.environ.get("TILEXR_VALIDATION_JOBS", "4")) if action == "build" else 4
        self.timeout = int(os.environ.get("TILEXR_VALIDATION_TIMEOUT", "180")) if self.is_run else 180
        self.server_count = None
        if self.is_run and os.environ.get("TILEXR_VALIDATION_SERVERS") is not None:
            self.server_count = int(os.environ["TILEXR_VALIDATION_SERVERS"])
            if not 2 <= self.server_count <= 32:
                raise ValueError("-n 必须为 2..32 台服务器")
        self.dedup = 1
        self.profiling = False
        self.core_overrides = {}
        if action == "build":
            for operator in ("dispatch", "combine"):
                value = os.environ.get(f"TILEXR_VALIDATION_{operator.upper()}_SEND_CORES", "")
                allowed = tuple(str(cores) for cores in range(16, 57 if operator == "combine" else 49, 4))
                if value:
                    if value not in allowed:
                        raise ValueError(f"Invalid {operator}_send_cores={value}; expected {'/'.join(allowed)}")
                    self.core_overrides[operator + "_send_core_num"] = int(value)
            value = os.environ.get("TILEXR_VALIDATION_DISPATCH_COUNT_CORES", "")
            if value:
                if value not in tuple(str(cores) for cores in range(1, 65)):
                    raise ValueError(f"Invalid dispatch_count_cores={value}; expected 1..64")
                self.core_overrides["dispatch_count_core_num"] = int(value)
            dedup = os.environ.get("TILEXR_VALIDATION_DEDUP", "auto")
            if dedup not in ("auto", "0", "1"):
                raise ValueError("编译 dedup 必须为 auto/0/1")
            self.dedup = "auto" if dedup == "auto" else int(dedup)
            breakdown = os.environ.get("TILEXR_VALIDATION_BREAKDOWN", "0")
            if breakdown not in ("0", "1"):
                raise ValueError("编译 breakdown 必须为 0/1")
            self.profiling = breakdown == "1"
        if self.mode not in ('host', 'production'):
            raise ValueError("不支持的构建模式：" + self.mode)
        if not 1 <= self.jobs <= 256 or not 30 <= self.timeout <= 180:
            raise ValueError("JOBS 必须为 1..256，TIMEOUT 必须为 30..180 秒")
        if self.user and not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", self.user):
            raise ValueError("SSH_USER 格式错误")
        self.hostfile = self.hostfile.resolve()
        self.hostfile.relative_to(self.root)  # 同步契约：配置属于当前工作目录。
        self.install = self.root / "install" / "server-dedup"
        self.logs = Path(os.environ.get("TILEXR_VALIDATION_RESULT_ROOT", self.root / "run/validation")).resolve()
        self.chain = None
        if self.is_run:
            default_host = None
            if action != 'case-dry-run' and (not os.environ.get('TILEXR_COMM_ID')):
                default_host = self.hosts()[0]["host"]
            self.chain = chain_configuration(default_host, dry_run=action == "case-dry-run")
        if action in ('run', 'node') and self.mode != 'production':
            raise ValueError("实际设备执行要求 production 构建；Host 模式可使用 case-dry-run")

    def hosts(self):
        try:
            hosts = (hosts_from_json(os.environ["TILEXR_DIRECT_HOSTS_JSON"])
                     if self.is_run and os.environ.get("TILEXR_DIRECT_HOSTS_JSON")
                     else hosts_from_file(self.hostfile))
            if self.server_count is not None:
                if self.server_count > len(hosts):
                    raise ValueError(f"-n {self.server_count} 超过 hostfile 中的 {len(hosts)} 台服务器")
                hosts = hosts[:self.server_count]
            if self.is_run:
                rank_mapping(hosts)
            if self.chain is not None:
                ranks = len(hosts) * 8
                experts = self.chain["moe_expert_num"]
                if not 0 < experts <= 1024 or experts % ranks or self.chain["topk"] > experts:
                    raise ValueError("chain 配置要求 0<moe_expert_num<=1024，能被 rank_size 整除，topk<=moe_expert_num")
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
    if config.get("server_dedup_layout_enabled", config.get("server_dedup_enabled")) is False:
        return (index + 479) // 480 * 512
    if index % 480 + 8 * k > 480:
        index = (index + 479) // 480 * 480
    payload = index + 8 * k
    return (payload + 479) // 480 * 512


def chain_configuration(default_host=None, *, dry_run=False):
    route_mode = os.environ.get("TILEXR_VALIDATION_ROUTE_MODE", "balance_shift")
    if route_mode not in ("random", "balance", "balance_shift"):
        raise ValueError("route_mode 必须为 random、balance 或 balance_shift")
    seed = os.environ.get("TILEXR_VALIDATION_SEED", "2026")
    if not re.fullmatch(r"0|[1-9][0-9]{0,9}", seed) or int(seed) > (1 << 32) - 1:
        raise ValueError("seed 必须为 0..4294967295 的整数")
    shape = {name: int(os.environ.get("TILEXR_VALIDATION_" + name.upper(), default))
             for name, default in (("bs", "96"), ("h", "7168"), ("topk", "6"), ("moe_expert_num", "96"))}
    if any(not 1 <= value <= (1 << 63) - 1 for value in shape.values()) or shape["topk"] > 32:
        raise ValueError("chain 配置 shape 必须是正 int64，topk<=32")
    dtype = os.environ.get("TILEXR_VALIDATION_DTYPE", "bf16")
    operator_mode = os.environ.get("TILEXR_VALIDATION_OPERATOR_MODE", "dispatch")
    if operator_mode not in ("dispatch", "combine", "both"):
        raise ValueError("OPERATOR_MODE 必须为 dispatch/combine/both")
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
        raise ValueError("chain 配置 dtype 必须为 bf16/fp16，block_dim 必须为 auto 或正 uint32")
    run_mode, loops, warmup = "functional", 1, 0
    run_mode = os.environ.get("TILEXR_VALIDATION_RUN_MODE", "functional")
    if run_mode not in ("functional", "performance", "breakdown"):
        raise ValueError("RUN_MODE 必须为 functional/performance/breakdown")
    loops = int(os.environ.get("TILEXR_VALIDATION_LOOPS", "1"))
    warmup = int(os.environ.get("TILEXR_VALIDATION_WARMUP", "0"))
    if not 1 <= loops <= (1 << 32) - 1 or not 0 <= warmup <= (1 << 32) - 1:
        raise ValueError("loops 必须为正 uint32，warmup 必须为非负 uint32")
    if run_mode == 'breakdown':
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

    chain = dict(shape, dtype=dtype, operator_mode=operator_mode,
                 quant_mode=int(quant_mode), dispatch_quant_mode=int(quant_mode), combine_quant_mode=0,
                 fp8_format=fp8_format, block_dim=block_dim, run_mode=run_mode, loops=loops, warmup=warmup,
                 route_mode=route_mode, seed=int(seed))
    chain.update(implemented_through="M5", scope="full", combine_send_core_num=32)
    if dry_run:
        return chain

    comm_id = endpoint(os.environ.get("TILEXR_COMM_ID") or f"{default_host}:10177", "TILEXR_COMM_ID")
    host, port = comm_id.split(":")
    barrier_addr = endpoint(os.environ.get("TILEXR_DEMO_BARRIER_ADDR", f"{host}:{int(port) + 97}"),
                            "TILEXR_DEMO_BARRIER_ADDR")
    if comm_id == barrier_addr:
        raise ValueError("communicator 与 Host barrier 必须使用不同端口")
    scope = {"dispatch": "dispatch", "combine": "combine-m5", "both": "dispatch-combine-m5"}[operator_mode]
    chain.update(comm_id=comm_id, barrier_addr=barrier_addr, kernel_scope=scope)
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


    kernel = install / "lib/tilexr/kernels/tilexr_ep_combine_memory_server_dedup_kernel.o"
    inspect(["readelf", "-h", kernel], "combine-server-dedup-elf.log")

    symbols = inspect(["nm", "-D", "--defined-only", "--no-demangle", "--format=posix", library],
                      "symbols.log")
    # 公共接口采用 extern "C"；匹配完整动态函数符号名，不带参数列表或左括号。
    exported = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in ("T", "W"):
            exported.add(fields[0])
    required_symbols = ["TileXRMoeEpDispatchMemoryServerDedup", "TileXRMoeEpDispatchMemory"]
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


CORE_MACROS = {
    "dispatch_send_core_num": "TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM",
    "combine_send_core_num": "TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM",
    "dispatch_count_core_num": "TILEXR_EP_SERVER_DEDUP_COUNT_CORE_NUM",
}


def validate_core_config(value):
    if not isinstance(value, dict) or set(value) != set(CORE_MACROS):
        raise ValueError("缺少编译核数配置，请重新执行 build_all.sh 和 sync.sh")
    for key, cores in value.items():
        allowed = range(1, 65) if key == "dispatch_count_core_num" else range(
            16, 49 if key == "dispatch_send_core_num" else 57, 4)
        if type(cores) is not int or cores not in allowed:
            raise ValueError(f"非法编译核数 {key}={cores}，请重新构建/同步")
    return dict(value)


def compile_core_config(config, work, log, command):
    probe = work / "core-config"
    command(["g++", "-std=c++14", "-I" + str(config.root / "src/include"),
             "-I" + str(config.root / "src/ep/common"),
             *(f"-D{CORE_MACROS[key]}={value}" for key, value in config.core_overrides.items()),
             config.root / "tools/validation/core_config.cpp", "-o", probe], log / "core-config-build.log")
    return validate_core_config(json.loads(command([probe], log / "core-config.json")))


def dedup_policy(manifest):
    """Legacy manifests describe a fixed mode; auto reserves the dedup layout."""
    layout = manifest.get("server_dedup_enabled")
    if type(layout) is not bool:
        raise ValueError("缺少编译 dedup 布局，请重新执行 build_all.sh 和 sync.sh")
    policy = manifest.get("server_dedup_policy", "1" if layout else "0")
    if policy not in ("auto", "0", "1") or layout != (policy != "0"):
        raise ValueError("编译 dedup 策略/布局不一致，请重新执行 build_all.sh 和 sync.sh")
    return policy


def effective_dedup(manifest, bs):
    policy = dedup_policy(manifest)
    if policy == "auto" and (type(bs) is not int or bs <= 0):
        raise ValueError("自动 dedup 策略需要有效的原始 BS")
    return bs > 32 if policy == "auto" else policy == "1"


def verify_build(config, *, check_run_mode=True, dry_run=False):
    manifest = read_json(config.install / "manifest.json")
    cores = validate_core_config(manifest.get("server_dedup_core_config"))
    if type(manifest.get("server_dedup_enabled")) is not bool or manifest.get("server_dedup_protocol") != 2:
        raise ValueError("缺少编译 dedup 模式/协议版本，请重新执行 build_all.sh 和 sync.sh")
    policy = dedup_policy(manifest)
    config.dedup = "auto" if policy == "auto" else int(policy)
    if config.chain is not None:
        config.chain.update(server_dedup_policy=policy,
                            server_dedup_layout_enabled=manifest["server_dedup_enabled"],
                            server_dedup_enabled=effective_dedup(manifest, config.chain.get("bs")))
        # Runtime configuration follows installed binaries, never source defaults
        # or build-only environment variables left in the caller's shell.
        config.chain.update(cores)
    if manifest.get("checkpoint") is not None or manifest['mode'] not in (('host', 'production') if dry_run else (config.mode,)):
        raise ValueError("测试配置与构建不一致，请重新执行 build_all.sh 和 sync.sh")
    if manifest.get("server_dedup_combine_contract") != combine_contract(cores["combine_send_core_num"]):
        raise ValueError("Combine 88B launch / 16B GM tiling构建契约或模块身份不符，请重新执行 build_all.sh")
    if manifest.get("server_dedup_contract") != DISPATCH_ABI:
        raise ValueError("需要完整 Dispatch 120B ABI / 独立 GM tiling 产物，请重新 build_all.sh 和 sync.sh")
    profiling = manifest.get("server_dedup_profiling")
    if type(profiling) is not bool or (check_run_mode and config.chain is not None and
                                       profiling is not (config.chain["run_mode"] == "breakdown")):
        raise ValueError("设备打点开关与运行模式不一致：breakdown 请执行 bash build_all.sh breakdown=1，"
                         "普通模式请执行 bash build_all.sh；成功后执行 bash sync.sh")
    if profiling and manifest.get("server_dedup_profile_schema") != measurements.RECORDS_SCHEMA:
        raise ValueError("需要标准 Dispatch records.v1 打点产物，请执行 bash build_all.sh breakdown=1 和 bash sync.sh")
    if manifest["architecture"] != platform.machine():
        raise ValueError("节点 CPU 架构与构建不一致，不能直接分发二进制")
    artifacts = artifact_files(config.install)
    # Incremental deployments retain retired files; only this manifest's files
    # belong to the active package. Local build validation remains exact.
    if config.action in ("verify", "node"):
        artifacts = {name: artifacts.get(name) for name in manifest["artifacts"]}
    if artifacts != manifest["artifacts"]:
        raise ValueError("安装产物缺失或摘要不一致，请重新构建/同步")
    return manifest


def validation_build_directory(config):
    name = "server-dedup"
    name += "-profile-on" if config.profiling else "-profile-off"
    return config.root / "build" / (name + f"-dedup-{config.dedup}")


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
    build_deadline = time.monotonic() + 900
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
    source["content_sha256"] = source_content_sha256(config.root, source)
    stage = work / "install"
    log = config.logs / ("build-" + build_id)
    print(f"开始构建 {config.checkpoint or 'server-dedup'}，mode={config.mode}，日志：{log}", flush=True)
    for tool in ("cmake", "g++"):
        command([tool, "--version"], log / (tool + "-version.log"))
    cores = compile_core_config(config, work, log, command)
    print("编译核数：" + " ".join(f"{operator}={cores[operator + '_send_core_num']}/{64 - cores[operator + '_send_core_num']}"
                                for operator in ("dispatch", "combine")) +
          f" dispatch_count={cores['dispatch_count_core_num']} (trailing cores)", flush=True)
    combine_tests = COMBINE_TESTS
    if (config.root / ".git").exists():
        status = command(["git", "status", "--porcelain", "--untracked-files=normal"],
                         log / "source-status.log", cwd=config.root)
        source["working_tree_dirty"] = bool(status.strip())
        source["working_tree_status"] = status.splitlines()
    common = ["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_INSTALL_LIBDIR=lib", f"-DCMAKE_INSTALL_PREFIX={stage}",
              f"-DTILEXR_EP_SERVER_DEDUP_ENABLE={config.dedup}",
              *(f"-D{CORE_MACROS[key]}={value}" for key, value in cores.items())]
    if config.mode == "production":
        command(["cmake", "-S", config.root, "-B", work / "production", *common,
                 "-DTILEXR_BUILD_EP=ON", "-DBUILD_TESTING=OFF", "-DTILEXR_BUILD_TESTS=OFF",
                 f"-DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING={'ON' if config.profiling else 'OFF'}"],
                log / "production-configure.log", timeout=600)
        command(["cmake", "--build", work / "production", "-j", config.jobs,
                 "--target", "tilexr-ep"],
                log / "production-build.log", timeout=3600)
        command(["cmake", "--install", work / "production"], log / "production-install.log", timeout=180)
        kernels = ["dispatch", "combine", "dispatch_memory", "combine_memory", "dispatch_memory_server_dedup"]
        kernels.append("combine_memory_server_dedup")
        for kernel in kernels:
            if not (stage / f"lib/tilexr/kernels/tilexr_ep_{kernel}_kernel.o").is_file():
                raise ValueError(f"缺少 EP kernel：{kernel}")
    host_build = work / "host"
    chain = config.mode == "production"
    command(["cmake", "-S", config.root / "tests/ep", "-B", host_build, *common,
             "-DBUILD_TILEXR_EP_DEMO=OFF", "-DBUILD_TILEXR_EP_PLAN_MULTIRANK_TEST=OFF",
             f"-DBUILD_TILEXR_EP_SERVER_DEDUP_CHAIN_TEST={'ON' if chain else 'OFF'}",
             f"-DTILEXR_INSTALL_PREFIX={stage}",
             f"-DPython3_EXECUTABLE={sys.executable}"], log / "host-configure.log", timeout=600)
    build_targets = (combine_tests - CONSUMER_TESTS - {COMBINE_CONFIG_TEST}) | {COMBINE_DRY_RUN_PROGRAM}
    if chain:
        build_targets = build_targets | {CHAIN_PROGRAM}
    targets = ["--target", *sorted(build_targets)]
    command(["cmake", "--build", host_build, "-j", config.jobs, *targets], log / "host-build.log", timeout=1800)
    if chain:
        (stage / "bin").mkdir(parents=True, exist_ok=True)
        shutil.copy2(host_build / CHAIN_PROGRAM, stage / "bin" / CHAIN_PROGRAM)
    if chain and not (stage / "bin" / CHAIN_PROGRAM).is_file():
        raise ValueError(f"Server Dedup 测试未安装：{CHAIN_PROGRAM}")
    try:
        selection = ["-R", "^(" + "|".join(sorted(combine_tests)) + ")$"]
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
        if test["name"] in CONSUMER_TESTS:
            if (len(command) != 4 or not command[1].endswith(".py") or command[2] != "--consumer"
                    or Path(command[3]).name != COMBINE_DRY_RUN_PROGRAM):
                raise ValueError(f"无法导出 consumer 测试命令：{command}")
            (stage / "bin").mkdir(parents=True, exist_ok=True)
            shutil.copy2(command[3], stage / "bin" / COMBINE_DRY_RUN_PROGRAM)
            argv = ["@python", str(Path(command[1]).resolve().relative_to(config.root)), "--consumer",
                    str(config.install.relative_to(config.root) / "bin" / COMBINE_DRY_RUN_PROGRAM)]
        elif len(command) == 2 and command[1].endswith(".py"):
            argv = ["@python", str(Path(command[1]).resolve().relative_to(config.root))]
        elif len(command) == 1:
            name = Path(command[0]).name
            (stage / "bin").mkdir(parents=True, exist_ok=True)
            shutil.copy2(command[0], stage / "bin" / name)
            if not (stage / "bin" / name).is_file():
                raise ValueError(f"Host 测试未安装：{name}")
            argv = [str(config.install.relative_to(config.root) / "bin" / name)]
        else:
            raise ValueError(f"无法导出测试命令：{command}")
        tests.append({"name": test["name"], "argv": argv})
    names = [test["name"] for test in tests]
    required = combine_tests
    missing = required - set(names)
    if missing or len(names) != len(set(names)):
        raise ValueError(f"必需测试缺失或重复：{sorted(missing)}")
    if config.mode == 'host':
        mapping_dir = stage / "share/tilexr/profiling"
        mapping_dir.mkdir(parents=True, exist_ok=True)
        for filename in ("server_dedup.mapping.json", "server_dedup_combine.mapping.json"):
            shutil.copy2(config.root / "src/ep/profiling" / filename, mapping_dir / filename)
    runtime_check(config, stage, log)
    final_source = inventory(config.root, exclude=(config.hostfile.relative_to(config.root).as_posix(),))
    if (final_source["commit"] != source["commit"] or
            source_content_sha256(config.root, final_source) != source["content_sha256"]):
        raise Failure("BUILD_FAILED", "构建期间源码发生变化，拒绝关联不确定产物")
    manifest = {"server_dedup_policy": str(config.dedup),
                "server_dedup_enabled": str(config.dedup) != "0", "server_dedup_protocol": 2,
                "server_dedup_core_config": cores,
                "build_id": build_id, "source": source, "mode": config.mode,
                "checkpoint": config.checkpoint, "architecture": platform.machine(),
                "python": sys.version, "cann": os.environ.get("ASCEND_HOME_PATH", ""),
                "driver": os.environ.get("ASCEND_DRIVER_PATH", ""), "tests": tests,
                "artifacts": artifact_files(stage)}
    manifest["build_timeout_seconds"] = 900
    manifest["server_dedup_combine_contract"] = combine_contract(cores["combine_send_core_num"])
    for operator, filename in (("dispatch", "server_dedup.mapping.json"), ("combine", "server_dedup_combine.mapping.json")):
        if not (stage / "share/tilexr/profiling" / filename).is_file():
            raise ValueError("缺少独立profiling mapping：" + operator)
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
    write_json(config.root / ".tilexr-source.json", source)
    (config.root / ".tilexr-validation-root").write_text("TileXR validation deployment\n", encoding="utf-8")
    print(f"BUILD_PASS commit={source['commit'] or 'unknown'} "
          f"mode={config.mode} tests={len(tests)}\n日志：{log}", flush=True)


def remote_argv(config, host, action, *arguments):
    command = "bash -s -- " + " ".join(shlex.quote(str(value)) for value in (config.root, action, *arguments))
    if action in ("verify", "node", "cancel"):
        values = {
            "TILEXR_VALIDATION_BUILD_MODE": config.mode,
            "TILEXR_VALIDATION_RESULT_ROOT": str(config.logs),
            "TILEXR_VALIDATION_BREAKDOWN": int(config.install.name.endswith("-profile-on")),
        }
        if action == "node":
            values.update(TILEXR_VALIDATION_SERVERS=config.server_count,
                          TILEXR_DIRECT_HOSTS_JSON=json.dumps(config.hosts(), separators=(",", ":")))
            if config.chain:
                for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "quant_mode", "operator_mode", "dispatch_quant_mode", "combine_quant_mode", "fp8_format",
                            "block_dim", "loops", "warmup", "run_mode", "msprof", "route_mode", "seed"):
                    values["TILEXR_VALIDATION_" + key.upper()] = config.chain.get(key)
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
    if not 2 <= len(hosts) <= 32 or any(len(host["devices"]) != 8 for host in hosts):
        raise ValueError("多 rank 配置要求 2 至 32 台主机，每台恰好 8 个 device ID，最多 256P")
    return [{"host": host["host"], "host_index": index, "local_rank": local_rank,
             "device": device, "rank": index * 8 + local_rank, "rank_size": len(hosts) * 8}
            for index, host in enumerate(hosts) for local_rank, device in enumerate(host["devices"])]


def rank_record(mapping, run_id, manifest):
    return dict(mapping, run_id=run_id, build_id=manifest["build_id"], commit=manifest["source"]["commit"],
                checkpoint=manifest["checkpoint"], status="PASS")


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
    validate_dispatch_record(dict(dispatch, chain=chain), mapping, run_id, manifest,
                             dict(config, kernel_scope="dispatch"),
                             invalid_output=allow_invalid_output and dispatch.get("validation_status") == "FAIL")

def validate_dispatch_record(record, mapping, run_id, manifest, chain_config, *, invalid_output=False):
    """Check completed Dispatch data, route tables and runtime identity directly."""
    linked = record["chain"]
    wanted = rank_record(mapping, run_id, manifest)
    for marker in (record, linked):
        expected = dict(wanted, status="FAIL") if marker is record and invalid_output else wanted
        if any(marker.get(key) != value for key, value in expected.items()):
            raise Failure("RESULT_FAILED", "Dispatch rank identity mismatch", rank=mapping["rank"])
        if marker.get("pid") != linked.get("pid"):
            raise Failure("RESULT_FAILED", "Dispatch process identity mismatch", rank=mapping["rank"])
    if chain_config is None:
        raise Failure("RESULT_FAILED", "缺少本次 Dispatch 配置", rank=mapping["rank"])
    for marker in (record, linked):
        enabled = marker.get("server_dedup_enabled")
        if (("server_dedup_enabled" in manifest and enabled is not effective_dedup(manifest, marker["bs"])) or type(enabled) is not bool or enabled is not linked.get("server_dedup_enabled") or
                ("server_dedup_enabled" in chain_config and
                 enabled is not chain_config["server_dedup_enabled"])):
            raise Failure("RESULT_FAILED", "Dispatch 实际去重状态缺失或 Rank 记录不一致", rank=mapping["rank"])
    for key in ("quant_mode", "fp8_format", "kernel_scope"):
        if record.get(key) != chain_config[key]:
            raise Failure("RESULT_FAILED", "Dispatch 实际类型或范围不一致", rank=mapping["rank"], field=key)
    stride = source_send_stride(dict(chain_config,
        server_dedup_layout_enabled=manifest.get("server_dedup_enabled", record["server_dedup_enabled"])))
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
    if any(type(record.get(key)) is not int or record[key] != loops for key in ("submitted", "completed")) or record.get("cleanup") != "complete":
        raise Failure("RESULT_FAILED", "Dispatch launch completion or cleanup missing", rank=mapping["rank"])
    for marker in (record, linked):
        if any(type(marker.get(key)) is not type(chain_config[key]) or marker[key] != chain_config[key]
               for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "route_mode", "seed")):
            raise Failure("RESULT_FAILED", "Dispatch shape/dtype/routing identity mismatch", rank=mapping["rank"])
    for marker in (record, linked):
        if any(marker.get(key) != chain_config[key] for key in ("run_mode", "loops", "warmup")):
            raise Failure("RESULT_FAILED", "rank 的模式或批次配置不一致", rank=mapping["rank"])
    if any(type(record.get(key)) is not int or record[key] != chain_config["warmup"]
           for key in ("warmup_submitted", "warmup_completed")):
        raise Failure("RESULT_FAILED", "rank 未完成指定预热批次", rank=mapping["rank"])


def validate_multi_magic(record, config, *, dispatch_only=False):
    """Validate caller-derived magic without consuming another communicator round."""
    total = config["loops"] + config["warmup"]
    both = config["operator_mode"] == "both"
    numbers = {"stream_synchronizations": 1}
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
        numbers.update(warmup_submitted=config["warmup"], warmup_completed=config["warmup"], combine_delay_us=0)
    for key, value in numbers.items():
        if type(record.get(key)) is not int or record[key] != value:
            raise Failure("RESULT_FAILED", "提交/完成、预热、magic/选窗或同步记录不一致", field=key)


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
                raise Failure("LOG_PARSE_FAILED", "Combine invalid result lacks completed Dispatch identity", rank=mapping["rank"])
            validate_combine_dispatch_record(dispatch, record["chain"], mapping, run_id, manifest, config,
                                            allow_invalid_output=True)
            validate_multi_magic(dispatch, config, dispatch_only=True)
        return
    independent = not both or config["dispatch_quant_mode"] == 4
    flags = dict(combine_empty=False, combine_inputs_unchanged=not both, combine_y_out_validated=True,
                 independent_expert_out_unchanged=independent, five_tables_linked=both, send_counts_linked=both)
    source = ("separate_expert_out" if independent else "dispatch_expand_x") if both else "standalone_expert_out"
    if (any(record.get(key) is not value for key, value in flags.items()) or
            record.get("kernel_scope") != config["kernel_scope"] or
            record.get("expert_out_source") != source or
            record.get("combine_input_check") != ("dispatch-final-reference" if both else "full-snapshot")):
        raise Failure("RESULT_FAILED", "输入检查范围、生命周期或原指针关联记录不一致", rank=mapping["rank"])
    if both:
        dispatch = record.get("dispatch")
        if not isinstance(dispatch, dict):
            raise Failure("LOG_PARSE_FAILED", "联合模式缺少真实 Dispatch PASS", rank=mapping["rank"])
        validate_combine_dispatch_record(dispatch, record["chain"], mapping, run_id, manifest, config, allow_invalid_output=invalid_output)
        validate_multi_magic(dispatch, config, dispatch_only=True)
    elif "dispatch" in record:
        raise Failure("RESULT_FAILED", "Combine 独跑意外提交 Dispatch", rank=mapping["rank"])


def validate_combine_return_record(record, mapping, config, *, invalid_output=False):
    for marker in (record, record["chain"]):
        for key in ("implemented_through", "scope"):
            if marker.get(key) != config[key]:
                raise Failure("RESULT_FAILED", "Combine module/configuration identity mismatch", field=key, rank=mapping["rank"])
    if config["operator_mode"] == "dispatch":
        return
    if record.get("native_status") != "combine-m5":
        raise Failure("RESULT_FAILED", "Combine native implementation identity mismatch", rank=mapping["rank"])
    if invalid_output:
        if (record.get("execution_completed") is not True or
                not isinstance(record.get("validation_error"), str) or not record["validation_error"] or
                not isinstance(record.get("validation_stage"), str) or not record["validation_stage"]):
            raise Failure("LOG_PARSE_FAILED", "Combine invalid output lacks execution completion/error evidence", rank=mapping["rank"])
    else:
        flags = ("combine_stage_validated", "combine_control_validated", "combine_control_reserved_unchanged",
                 "combine_result_validated", "combine_unwritten_results_unchanged")
        flags += ("combine_y_out_validated", "combine_result_flags_cleared")
        flags += ("combine_partial_validated",)
        if any(record.get(key) is not True for key in flags):
            raise Failure("RESULT_FAILED", "Combine stage/control/return payload/Flag/protection validation missing", rank=mapping["rank"])
    values = [record.get(key) for key in ("combine_type3_rows", "combine_type1_rows", "combine_token_num", "combine_direct_received_rows")]
    if (any(type(value) is not int or not 0 <= value <= 10000 for value in values[:3]) or
            type(values[3]) is not int or not 0 <= values[3] <= config["bs"] * config["topk"]):
        raise Failure("RESULT_FAILED", "Combine row counts invalid", rank=mapping["rank"])
    stage_rows, direct_rows, tokens, received = values
    if stage_rows + direct_rows > tokens:
        raise Failure("RESULT_FAILED", "Combine disjoint row counts exceed TokenNum", rank=mapping["rank"])
    partial = record.get("combine_type2_rows")
    partial_received = record.get("combine_partial_received_rows")
    if (type(partial) is not int or not 0 <= partial <= tokens - stage_rows - direct_rows or
            type(partial_received) is not int or not 0 <= partial_received <= config["bs"] * config["topk"] or
            received + partial_received > config["bs"] * config["topk"]):
        raise Failure("RESULT_FAILED", "Combine partial row counts invalid", rank=mapping["rank"])
    if (record.get("combine_numeric_policy") != "fp32-sequential-rne-bit-exact" or
            record.get("combine_numeric_atol") != 0 or record.get("combine_numeric_rtol") != 0):
        raise Failure("RESULT_FAILED", "Combine numerical policy mismatch", rank=mapping["rank"])
    try:
        operator_measure.return_traffic(record, config)
    except ValueError as error:
        raise Failure("RESULT_FAILED", str(error), rank=mapping["rank"]) from error
    numbers = dict(combine_checked_window_mask=record.get("combine_window_mask"))
    observed = record.get("combine_observed_rounds")
    if type(observed) is not int or observed not in ((0, 1) if invalid_output else (1,)):
        raise Failure("RESULT_FAILED", "Combine observation count does not match result status", rank=mapping["rank"])
    if any(type(record.get(key)) is not int or record[key] != value for key, value in numbers.items()):
        raise Failure("RESULT_FAILED", "Combine traffic/observation/window evidence mismatch", rank=mapping["rank"])


def validate_combine_chain_record(record, mapping, run_id, manifest, config, *, invalid_output=False):
    """Validate current full Combine and its linked Dispatch evidence."""
    if config is None:
        raise Failure("RESULT_FAILED", "缺少 Combine 配置", rank=mapping["rank"])
    for marker in (record, record["chain"]):
        for key in ("operator_mode", "dispatch_quant_mode", "combine_quant_mode", "quant_mode",
                    "fp8_format", "run_mode", "loops", "warmup"):
            if type(marker.get(key)) is not type(config[key]) or marker[key] != config[key]:
                raise Failure("RESULT_FAILED", "Combine 配置回显不一致", rank=mapping["rank"], field=key)
    validate_combine_return_record(record, mapping, config, invalid_output=invalid_output)
    validate_combine_multi_record(record, mapping, run_id, manifest, config, invalid_output=invalid_output)


def validate_rank_records(records, expected, run_id, manifest, chain_config=None, *, check_measurements=True,
                          allow_invalid_output=False, dry_run=False):
    if not isinstance(records, list) or any(not isinstance(record, dict) for record in records):
        raise Failure("LOG_PARSE_FAILED", "rank 记录格式错误")
    if not dry_run and chain_config is None:
        raise Failure("RESULT_FAILED", "缺少本次运行配置")
    actual = [record.get("rank") for record in records]
    if (len(records) != len(expected) or any(type(rank) is not int for rank in actual)
            or set(actual) != {mapping["rank"] for mapping in expected}):
        raise Failure("RESULT_FAILED", "rank 缺失、重复或不属于本节点")
    for record, mapping in zip(sorted(records, key=lambda item: item["rank"]), expected):
        wanted = rank_record(mapping, run_id, manifest)
        invalid_output = (allow_invalid_output and chain_config is not None
                          and record.get("validation_status") == "FAIL")
        chain = not dry_run
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
            if "server_dedup_enabled" in manifest:
                for marker in (record, linked):
                    if marker.get("server_dedup_enabled") is not effective_dedup(manifest, marker["bs"]):
                        raise Failure("RESULT_FAILED", "实际 dedup 模式与安装包策略不一致", rank=mapping["rank"])
            chain_config = dict(chain_config, server_dedup_enabled=linked["server_dedup_enabled"],
                                rank_size=mapping["rank_size"])
            validate_combine_chain_record(record, mapping, run_id, manifest, chain_config, invalid_output=invalid_output)
            if check_measurements:
                try:
                    operator_measure.validate_operator_measurements(record, chain_config, run_id=run_id,
                        kernel_shas=operator_kernel_shas(manifest, chain_config))
                except (ValueError, TypeError, KeyError) as error:
                    raise Failure("RESULT_FAILED", f"算子测量记录无效：{error}", rank=mapping["rank"]) from error
            loops = chain_config["loops"]
            expected_peer_bytes = SERVER_DEDUP_PEER_BYTES
            if type(linked.get("peer_mem_bytes")) is not int or linked["peer_mem_bytes"] != expected_peer_bytes:
                raise Failure("RESULT_FAILED", "rank peer window 容量不符合当前配置",
                              rank=mapping["rank"], expected_bytes=expected_peer_bytes,
                              actual_bytes=linked.get("peer_mem_bytes"))
            if (linked.get("peer_count") != mapping["rank_size"] or
                    linked["pid"] != record["pid"] or any(type(record.get(key)) is not int or record[key] != loops
                        for key in ("submitted", "completed")) or record.get("cleanup") != "complete"):
                raise Failure("RESULT_FAILED", "rank 未完成 peer window 检查、指定轮数 launch 或资源清理",
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
        if not line.startswith(OPERATOR_BREAKDOWN_MARKER):
            continue
        try:
            marker = json.loads(line[len(OPERATOR_BREAKDOWN_MARKER):])
        except ValueError as error:
            raise Failure("LOG_PARSE_FAILED", "算子breakdown marker不是有效JSON") from error
        operator = marker.get("operator") if isinstance(marker, dict) else None
        if not isinstance(operator, str) or operator not in expected or operator in markers:
            raise Failure("LOG_PARSE_FAILED", "算子breakdown marker重复或包含未启用算子")
        wanted = dict(rank_record(mapping, run_id, manifest), pid=record["pid"], operator=operator,
                      schema=measurements.RECORDS_SCHEMA, records_path=str(records_paths[operator]),
                      capture_id=capture_id, launches=config["warmup"] + config["loops"],
                      capture_mode="latest_launch", captured_launches=1,
                      captured_launch=config["warmup"] + config["loops"] - 1)
        if any(type(marker.get(key)) is not type(value) or marker[key] != value for key, value in wanted.items()):
            raise Failure("RESULT_FAILED", "算子breakdown身份/路径/批次不匹配", rank=mapping["rank"], operator=operator)
        markers[operator] = marker
    if set(markers) != expected:
        raise Failure("LOG_PARSE_FAILED", "缺少启用算子的breakdown marker", rank=mapping["rank"])
    return markers


def consumer_wall_time(output, rank, process_seconds):
    """Optional diagnostics from this process only; never a correctness verdict."""
    prefix = "EP_SERVER_DEDUP_WALL "
    rows = [line[len(prefix):] for line in output.splitlines() if line.startswith(prefix)]
    if not rows:
        return None  # Older installed consumers and killed processes may omit it.
    try:
        if len(rows) != 1:
            raise ValueError("duplicate consumer wall-time records")
        timing = json.loads(rows[0])
        if type(timing["rank"]) is not int or timing["rank"] != rank or timing["unit"] != "seconds":
            raise ValueError("consumer wall-time identity mismatch")
        stages = timing["stages"]
        if not isinstance(stages, dict) or not stages or any(
                not re.fullmatch(r"[a-z][a-z0-9-]{0,63}", name) for name in stages):
            raise ValueError("invalid consumer wall-time stages")
        values = [timing["total"], *stages.values()]
        if any(type(value) not in (int, float) or not math.isfinite(value) or value < 0 for value in values):
            raise ValueError("invalid consumer wall-time duration")
        if not math.isclose(sum(stages.values()), timing["total"], rel_tol=1e-6, abs_tol=1e-6):
            raise ValueError("consumer wall-time stages do not sum to total")
        if timing["total"] > process_seconds + 0.001:
            raise ValueError("consumer duration exceeds enclosing process")
        timing.update(status="recorded", outside_consumer=max(0.0, process_seconds - timing["total"]))
        return timing
    except (ValueError, TypeError, KeyError, OverflowError) as error:
        return dict(status="invalid", error=str(error))


def run_ranks(config, run_id, manifest, mappings, deadline, cancelled, result):
    def launch(mapping):
        rank = mapping["rank"]
        timer = PhaseTimer("prepare")
        log = config.logs / run_id / f"rank-{rank}.log"
        output = None
        record, communication_completed = None, False
        try:
            if cancelled.is_set():
                return None
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("EXIT_FAILED", "rank 启动前已超过节点执行期限", timeout=True)
            env = None
            argv = [config.install / "bin" / CHAIN_PROGRAM, run_id, manifest["build_id"],
                    manifest["source"]["commit"] or "unknown",
                    *[str(mapping[key]) for key in ("host", "host_index", "local_rank", "device", "rank", "rank_size")]]
            env = dict(os.environ)
            for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "quant_mode", "operator_mode", "dispatch_quant_mode", "combine_quant_mode", "fp8_format", "loops", "warmup", "run_mode", "route_mode", "seed"):
                env["TILEXR_VALIDATION_" + key.upper()] = str(config.chain[key])
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
                for operator in operator_measure.active_operators(config.chain):
                    records_paths[operator] = log.parent / f"rank-{rank}.{operator}.records.v1"
                    key = "TILEXR_PROFILING_RECORDS_PATH" if operator == "dispatch" else "TILEXR_PROFILING_COMBINE_RECORDS_PATH"
                    env[key] = str(records_paths[operator])
            if config.chain["run_mode"] == "performance":
                profile_dir = log.parent / f"msprof-rank-{rank}"
                argv = [config.chain["msprof"], "--application=" + shlex.join([str(arg) for arg in argv]),
                        "--output=" + str(profile_dir)]
            timer.next("process")
            output = execute(argv, log, timeout=remaining, cwd=config.root, env=env, cancelled=cancelled)
            timer.next("validate")
            marker = DISPATCH_MARKER
            if config.chain['operator_mode'] != 'dispatch':
                marker = COMBINE_CHAIN_MARKER
            dispatch_invalid = any(line.startswith(DISPATCH_INVALID_MARKER) for line in output.splitlines())
            combine_invalid = any(line.startswith(COMBINE_INVALID_MARKER) for line in output.splitlines())
            if dispatch_invalid:
                if any(line.startswith(DISPATCH_MARKER) for line in output.splitlines()):
                    raise Failure("LOG_PARSE_FAILED", "Dispatch 同时报告 PASS 和 INVALID")
                if config.chain["operator_mode"] == "dispatch":
                    marker = DISPATCH_INVALID_MARKER
            if combine_invalid:
                if config.chain['operator_mode'] == 'dispatch':
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
            has_dispatch = dispatch_invalid or any(line.startswith(DISPATCH_MARKER) for line in output.splitlines())
            if config.chain["operator_mode"] == "both":
                record["dispatch"] = parse_rank_marker(output, DISPATCH_INVALID_MARKER if dispatch_invalid else DISPATCH_MARKER)
            elif config.chain["operator_mode"] == "combine" and has_dispatch:
                raise Failure("RESULT_FAILED", "Combine 独跑意外出现 Dispatch PASS")
            elif config.chain["operator_mode"] == "dispatch" and any(
                    line.startswith(COMBINE_CHAIN_MARKER) for line in output.splitlines()):
                raise Failure("RESULT_FAILED", "Dispatch 独跑意外出现 Combine PASS")
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
                        timer.next("profile_decode")
                        evidence = operator_measure.performance_record(profile_dir, config.chain["loops"],
                            config.chain["warmup"], operator=operator, run_id=run_id, rank=rank,
                            kernel_sha=kernel_shas[operator],
                            config=config.chain, record=record)
                    else:
                        timer.next("profile_wait")
                        with profiling_lock:
                            timer.next("profile_decode")
                            # The node's chain config has no topology; use the
                            # selected global rank mapping, not local worker count.
                            evidence = operator_measure.breakdown_record(records_paths[operator],
                                dict(config.chain, rank_size=mapping["rank_size"]), rank,
                                env["TILEXR_PROFILING_CAPTURE_ID"], log.parent / f"profile-rank-{rank}" / operator,
                                operator=operator, run_id=run_id, kernel_sha=kernel_shas[operator],
                                mapping_path=config.install / ("share/tilexr/profiling/server_dedup.mapping.json" if operator == "dispatch"
                                    else "share/tilexr/profiling/server_dedup_combine.mapping.json"),
                                record=record)
                    record["operator_measurements"][operator] = {config.chain["run_mode"]: evidence}
                except (ValueError, TypeError, KeyError, OSError) as error:
                    capture_errors.extend(failure_records(error, "RESULT_FAILED", rank=rank, operator=operator))
            if capture_errors:
                raise Failure("RESULT_FAILED", "算子测量采集失败；保留其他算子已完成报告", additional_failures=capture_errors)
            timer.next("validate")
            validate_rank_records([record], [mapping], run_id, manifest, config.chain, allow_invalid_output=True)
            return record
        except Exception as error:
            capture = dict(communication_completed=True) if communication_completed else {}
            if communication_completed and config.chain["run_mode"] == "breakdown":
                capture.update(operator_records_paths={operator: str(path) for operator, path in records_paths.items()},
                               report_dir=str(log.parent / f"profile-rank-{rank}"))
            failures = failure_records(error, "LAUNCH_FAILED", rank=rank, log=str(log), **capture)
            # 保留第一个观察到的失败；其他 rank 的取消/清理错误作为后续证据。
            with error_lock:
                if communication_completed:
                    result.setdefault("profiling_failures", []).append(dict(rank=rank, communication=record, **capture))
                result["errors"].extend(failures)
                cancelled.set()
            return None
        finally:
            timing = dict(rank=rank, **timer.snapshot())
            try:
                captured = output if output is not None else log.read_text(encoding="utf-8", errors="replace")
                consumer = consumer_wall_time(captured, rank, timing["stages"].get("process", 0.0))
                if consumer is not None:
                    timing["consumer"] = consumer
            except OSError:
                pass  # Preserve the original launch/log error and partial timer.
            with error_lock:
                result["rank_wall_times"].append(timing)

    result["rank_wall_times"] = []
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
    result["rank_wall_times"].sort(key=lambda record: record["rank"])
    # Node status describes execution/capture; output correctness is reported
    # separately so a completed experiment cannot cancel the other nodes.
    result["validation_status"] = "FAIL" if any(
        record.get("validation_status") == "FAIL" for record in result["ranks"]) else (
        "PASS" if not result["errors"] else "UNKNOWN")


def node_tests(manifest):
    # The manifest is the installed catalog, not a prerequisite suite for NPU runs.
    return []


def run_validation_test(config, test, log, timeout, *, device=None, cancelled=None):
    name = test["name"]
    argv = [sys.executable if value == "@python" else value if value.startswith("--") else str(config.root / value)
            for value in test["argv"]]
    env = None
    path = log / (name + ".log")
    output = execute(argv, path, timeout=timeout, cwd=config.root, env=env, cancelled=cancelled)
    prefix = "test_tilexr_ep_server_dedup_"
    marker = None
    markers_by_name = {
        COMBINE_PROFILE_KERNEL_TEST: ("EP_SERVER_DEDUP_COMBINE_PROFILE_KERNEL_", ""),
        COMBINE_PROFILE_REGIONS_TEST: ("EP_SERVER_DEDUP_COMBINE_PROFILE_REGIONS_", ""),
        "test_tilexr_ep_server_dedup_combine_m5": ("EP_SERVER_DEDUP_COMBINE_M5_", ""),
        DISPATCH_CONSUMER_TEST: ("EP_SERVER_DEDUP_DISPATCH_CONSUMER_", " test=lifecycle"),
        COMBINE_CONSUMER_TEST: ("EP_SERVER_DEDUP_COMBINE_CONSUMER_", " test=lifecycle"),
        COMBINE_CONFIG_TEST: ("EP_SERVER_DEDUP_COMBINE_CONFIG_", ""),
        COMBINE_DRY_RUN_TEST: ("EP_SERVER_DEDUP_COMBINE_DRYRUN_", " test=configuration"),
    }
    if name in COMBINE_ABI_HOST_TESTS:
        marker, suffix = "EP_SERVER_DEDUP_COMBINE_ABI_HOST_", " test=" + name.removeprefix(prefix + "combine_")
    elif name in markers_by_name:
        marker, suffix = markers_by_name[name]
    elif name in DISPATCH_ABI_TESTS:
        marker, suffix = "EP_SERVER_DEDUP_ABI_HOST_", " test=" + name.removeprefix(prefix)
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
    result.update(build_timeout_seconds=900, validation_timeout_seconds=config.timeout,
                  per_test_timeout_seconds=120)
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
        tests = manifest["tests"]
        names = [test["name"] for test in tests]
        expected = COMBINE_TESTS
        if expected and not expected.issubset(names):
            raise ValueError(f"Combine 安装包测试集不完整：缺少 {sorted(expected - set(names))}；多余 {sorted(set(names) - expected)}")
        if not names or len(names) != len(set(names)):
            raise ValueError("安装包没有 Host/Python 测试或测试名称重复，请检查构建 manifest")
        result["expected_tests"] = names
        for test in tests:
            current_test = test["name"]
            if cancelled.is_set():
                raise InterruptedError("Host/Python 回归已取消")
            budget = config.timeout
            budget = min(120, started + config.timeout - time.monotonic())
            if budget <= 0:
                raise Failure("EXIT_FAILED", "Combine 验证超过总期限", timeout=True)
            run_validation_test(config, test, log, budget, cancelled=cancelled)
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


def case_dry_run(config):
    """Resolve the normal case/rank contract locally without starting SSH or ACL."""
    manifest = verify_build(config, check_run_mode=False, dry_run=True)
    mappings = rank_mapping(config.hosts())
    run_id = "dry-run-" + uuid.uuid4().hex
    result = dict(run_id=run_id, build_id=manifest["build_id"], checkpoint=config.checkpoint,
                  commit=manifest["source"]["commit"], mode="host", status="FAIL", ranks=[], errors=[],
                  scope="local configuration/rank dry-run; no device or network operations",
                  chain_configuration=dict(config.chain), rank_size=len(mappings), completed_ranks=0)
    deadline = time.monotonic() + config.timeout
    try:
        env = dict(os.environ)
        for key, value in config.chain.items():
            env["TILEXR_VALIDATION_" + key.upper()] = str(value)
        env.pop("TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM", None)
        if config.chain["block_dim"] != "auto":
            env["TILEXR_EP_DISPATCH_MEMORY_SERVER_DEDUP_BLOCK_DIM"] = config.chain["block_dim"]
        for mapping in mappings:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("EXIT_FAILED", "dry-run 超过总期限", timeout=True)
            argv = [config.install / "bin" / COMBINE_DRY_RUN_PROGRAM, "--dry-run", run_id,
                    manifest["build_id"], manifest["source"]["commit"] or "unknown",
                    *[str(mapping[key]) for key in ("host", "host_index", "local_rank", "device", "rank", "rank_size")]]
            output = execute(argv, config.logs / run_id / f"rank-{mapping['rank']}.log",
                             timeout=remaining, cwd=config.root, env=env)
            result["ranks"].append(parse_rank_marker(output, COMBINE_DRY_RUN_MARKER))
        validate_rank_records(result["ranks"], mappings, run_id, manifest, config.chain, dry_run=True)
        for record, mapping in zip(result["ranks"], mappings):
            if any(type(record.get(key)) is not type(value) for key, value in rank_record(mapping, run_id, manifest).items()):
                raise Failure("RESULT_FAILED", "rank 身份字段类型不一致", rank=mapping["rank"])
            for key in ("operator_mode", "dispatch_quant_mode", "combine_quant_mode", "quant_mode",
                        "fp8_format", "run_mode", "loops", "warmup"):
                if type(record.get(key)) is not type(config.chain[key]) or record[key] != config.chain[key]:
                    raise Failure("RESULT_FAILED", "配置回显不一致：" + key, rank=record["rank"])
            for key in ("device_allocated", "communicator_created", "kernel_launched", "network_used"):
                if record.get(key) is not False:
                    raise Failure("RESULT_FAILED", "未确认无副作用：" + key, rank=record["rank"])
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
    timer = PhaseTimer("verify")
    log = config.logs / identifier(run_id)
    log.mkdir(parents=True, exist_ok=True)
    result = {"run_id": run_id, "build_id": build_id, "host": socket.gethostname(),
              "checkpoint": config.checkpoint, "mode": config.mode, "status": "FAIL", "tests": [], "errors": []}
    write_json(log / "pid.json", {"pid": os.getpid(), "start": pid_identity(os.getpid())})
    print(f"NODE_STARTED run_id={run_id}", flush=True)
    cancelled = threading.Event()

    def interrupted(signum, frame):
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
        timer.next("host_tests")
        for test in node_tests(manifest):
            current_test = test["name"]
            if cancelled.is_set():
                raise InterruptedError("节点已收到取消信号")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("EXIT_FAILED", "节点测试超过执行期限", timeout=True)
            run_validation_test(config, test, log, remaining, device=device,
                                cancelled=cancelled)
            result["tests"].append({"name": test["name"], "status": "PASS"})
        current_test = None
        timer.next("ranks")
        mappings = rank_mapping(config.hosts())
        index = int(host_index) if host_index is not None else -1
        if not 0 <= index < len(mappings) // 8:
            raise ValueError("多 rank 配置缺少有效的 hostfile 节点序号")
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
        result["wall_time"] = timer.snapshot()
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
    timer = config.wall_timer = PhaseTimer("local_check")
    deadline = time.monotonic() + config.timeout
    execution_deadline = min(deadline - 20, time.monotonic() + 120)
    hosts = config.hosts()
    mappings = rank_mapping(hosts)

    def expected_ranks(index):
        return mappings[index * 8:(index + 1) * 8] if mappings is not None else None
    check_coordinator(config, hosts)
    run_id = uuid.uuid4().hex
    log = config.logs / run_id
    manifest = verify_build(config)

    timer.next("remote_check")
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

    timer.next("nodes")
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

    timer.next("collect")
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
            if not failures:
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
    timer.next("summary")
    scope = (f"{len(mappings)}P current {config.chain['operator_mode']} execution with independent correctness checks; "
             f"{config.chain['warmup']} warmup + {config.chain['loops']} measured launches; "
             f"mode={config.chain['run_mode']}; no performance threshold.")
    report = {"run_id": run_id, "commit": manifest["source"]["commit"], "build_id": manifest["build_id"],
              "source_provenance": manifest["source"]["provenance"],
              "checkpoint": config.checkpoint, "mode": config.mode, "status": "FAIL" if errors else "PASS",
              "expected_hosts": hosts, "server_count": len(hosts), "nodes": nodes, "primary_error": primary_error, "errors": errors,
              "scope": scope}
    rank_summary = ""
    # Reuse one validated selection for counts, measurements, traces and analysis.
    records = node_profile_records(nodes, run_id, manifest, mappings, config.chain) if mappings is not None else []
    if mappings is not None:
        completed = len(records)
        report.update(rank_size=len(mappings), completed_ranks=completed, rank_mapping=mappings)
        if config.chain is not None:
            report["chain_configuration"] = dict(config.chain)
            if config.chain['operator_mode'] != 'combine' and (not errors):
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
            if config.chain['run_mode'] != 'functional':
                full_config = dict(config.chain, rank_size=len(mappings))
                kernel_shas = operator_kernel_shas(manifest, config.chain)
                try:
                    report["operator_measurements"] = operator_measure.summarize_operator_measurements(
                        records, full_config, run_id, kernel_shas)
                except (ValueError, TypeError, KeyError, OSError) as error:
                    errors.append(Failure("RESULT_FAILED", f"算子测量汇总失败：{error}").record())
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
                        errors.append(Failure("LOG_COLLECT_FAILED", f"独立trace汇总失败：{error}").record())
                if errors:
                    primary_error = primary_error or errors[0]
                    report.update(status="FAIL", primary_error=primary_error)
        rank_summary = f"ranks={completed}/{len(mappings)} "
    report["wall_time"] = timer.snapshot()
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
    parser.add_argument("action", choices=("env", "build", "sync", "run", "host-validate", "case-dry-run", "node", "verify", "cancel"))
    parser.add_argument("arguments", nargs="*")
    args = parser.parse_args()
    try:
        if sys.version_info < (3, 9):
            raise ValueError("验证工具要求 Python 3.9 或更新版本")
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
                raise ValueError("sync 不接受额外参数")
            with operation_lock(config, ".operation.lock"):
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
        elif args.action == "node":
            with operation_lock(config, "node.lock"):
                return node(config, *args.arguments)
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
            timer = getattr(locals().get("config"), "wall_timer", None)
            if timer is not None:
                report["wall_time"] = timer.snapshot()
            write_json(directory / "failure.json", report)
            run_summary.write_result(directory, report, directory / "failure.json")
        for record in records:
            report_failure(record)
        return FAILURE_CODES[records[0]["type"]]


if __name__ == "__main__":
    sys.exit(main())
