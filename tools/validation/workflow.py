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
import run_summary

from source import inventory


CHECKPOINT = "ep-sdedup-p1-abi-host"
EMPTY_KERNEL_CHECKPOINT = "ep-sdedup-p1-empty-kernel"
DRY_RUN_CHECKPOINT = "ep-sdedup-p1-dry-run"
DRY_RUN_MARKER = "EP_SERVER_DEDUP_DRYRUN_PASS "
CHAIN_CHECKPOINT = "ep-sdedup-p1-chain-single"
CHAIN_MULTI_CHECKPOINT = "ep-sdedup-p1-chain-multi"
DISPATCH_CHECKPOINT = "ep-sdedup-p2-dispatch"
DISPATCH_MARKER = "EP_SERVER_DEDUP_DISPATCH_PASS "
DISPATCH_INVALID_MARKER = "EP_SERVER_DEDUP_DISPATCH_INVALID "
DISPATCH_ABI = {"launch_bytes": 192, "layout": "r07-token-prefix", "kernel_scope": "dispatch"}
MEMORY_BOUNDARY_CHECKPOINT = "ep-sdedup-p1-memory-boundary"
MEMORY_BOUNDARY_MARKER = "EP_SERVER_DEDUP_MEMORY_BOUNDARY_PASS "
SERVER_DEDUP_PEER_BYTES = 8 * 1024 ** 3
MEMORY_BOUNDARY_BYTES = 4 * 1024 ** 3
CHAIN_CHECKPOINTS = (DISPATCH_CHECKPOINT, MEMORY_BOUNDARY_CHECKPOINT)
CHAIN_PROGRAM = "test_tilexr_ep_server_dedup_chain"
CHAIN_MARKER = "EP_SERVER_DEDUP_CHAIN_PASS "
BREAKDOWN_MARKER = "EP_SERVER_DEDUP_BREAKDOWN_PASS "
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
        self.is_run = action in ("run", "node", "rank")
        self.root = Path(os.environ.get("TILEXR_VALIDATION_ROOT", Path(__file__).resolve().parents[2])).resolve()
        self.mode = os.environ.get("TILEXR_VALIDATION_BUILD_MODE", "production")
        # 同步和人工测试继承安装检查点；build_all.sh 选择当前生产增量。
        self.checkpoint = os.environ.get("TILEXR_VALIDATION_CHECKPOINT")
        if not self.checkpoint:
            installed = self.root / "install/validation/manifest.json"
            self.checkpoint = read_json(installed)["checkpoint"] if installed.exists() else CHECKPOINT
        self.hostfile = (self.root / "hostfile" if action in ("build", "verify", "host-validate") else
                         Path(os.environ.get("TILEXR_VALIDATION_HOSTFILE", self.root / "hostfile")))
        self.user = "" if action in ("build", "verify", "host-validate") else os.environ.get("TILEXR_VALIDATION_SSH_USER", "")
        self.jobs = int(os.environ.get("TILEXR_VALIDATION_JOBS", "4")) if action == "build" else 4
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
        if self.checkpoint in (CHAIN_CHECKPOINT, CHAIN_MULTI_CHECKPOINT, "ep-sdedup-p2-first-hop", "ep-sdedup-p2-source-send"):
            raise ValueError("旧 cnt-only/first-hop/source-send checkpoint 已由 ep-sdedup-p2-dispatch 替代；更新 CHECKPOINT 后执行 build_all.sh 和 sync.sh")
        if self.mode not in ("host", "production") or self.checkpoint not in (
                CHECKPOINT, EMPTY_KERNEL_CHECKPOINT, *RANK_CHECKPOINTS):
            raise ValueError("不支持的 checkpoint：" + self.checkpoint)
        if self.checkpoint in (EMPTY_KERNEL_CHECKPOINT, *CHAIN_CHECKPOINTS) and self.mode != "production":
            raise ValueError("empty-kernel / dispatch / memory-boundary 必须使用 production 模式验证真实 A5 artifact 和设备调用")
        if not 1 <= self.jobs <= 256 or not 30 <= self.timeout <= 180:
            raise ValueError("JOBS 必须为 1..256，TIMEOUT 必须为 30..180 秒")
        if self.user and not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", self.user):
            raise ValueError("SSH_USER 格式错误")
        self.hostfile = self.hostfile.resolve()
        self.hostfile.relative_to(self.root)  # 同步契约：配置属于当前工作目录。
        self.install = self.root / "install/validation"
        self.logs = Path(os.environ.get("TILEXR_VALIDATION_RESULT_ROOT", self.root / "run/validation")).resolve()
        self.chain = chain_configuration(self.checkpoint) if self.is_run and self.checkpoint in CHAIN_CHECKPOINTS else None

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
                if self.chain["bs"] * ranks % epoch:
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
    payload = (data + 31) // 32 * 32 + (32 + 16 * (k - 1) + 31) // 32 * 32
    return (payload + 479) // 480 * 512


def chain_configuration(checkpoint):
    if checkpoint == MEMORY_BOUNDARY_CHECKPOINT and (
            os.environ.get("TILEXR_VALIDATION_RUN_MODE", "functional") != "functional" or
            os.environ.get("TILEXR_VALIDATION_LOOPS", "1") != "1" or
            os.environ.get("TILEXR_VALIDATION_WARMUP", "0") != "0"):
        raise ValueError("memory-boundary 仅执行 functional，loops=1，warmup=0")
    shape = {name: int(os.environ.get("TILEXR_VALIDATION_" + name.upper(), default))
             for name, default in (("bs", "96"), ("h", "7168"), ("topk", "6"), ("moe_expert_num", "96"))}
    if any(not 1 <= value <= (1 << 63) - 1 for value in shape.values()) or shape["topk"] > 32:
        raise ValueError("chain 检查点 shape 必须是正 int64，topk<=32")
    dtype = os.environ.get("TILEXR_VALIDATION_DTYPE", "bf16")
    quant_mode = os.environ.get("TILEXR_VALIDATION_QUANT_MODE", "0")
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
    if checkpoint == DISPATCH_CHECKPOINT:
        run_mode = os.environ.get("TILEXR_VALIDATION_RUN_MODE", "functional")
        if run_mode not in ("functional", "performance", "breakdown"):
            raise ValueError("RUN_MODE 必须为 functional/performance/breakdown")
        loops = int(os.environ.get("TILEXR_VALIDATION_LOOPS", "1"))
        warmup = int(os.environ.get("TILEXR_VALIDATION_WARMUP", "0"))
        if not 1 <= loops <= (1 << 32) - 1 or not 0 <= warmup <= (1 << 32) - 1:
            raise ValueError("loops 必须为正 uint32，warmup 必须为非负 uint32")
        if run_mode == "breakdown":
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
            raise ValueError(f"请在 validation.local.sh 配置 {name}=首节点可跨机访问的IPv4:端口") from None

    comm_id = endpoint(os.environ.get("TILEXR_COMM_ID", ""), "TILEXR_COMM_ID")
    host, port = comm_id.split(":")
    barrier_addr = endpoint(os.environ.get("TILEXR_DEMO_BARRIER_ADDR", f"{host}:{int(port) + 97}"),
                            "TILEXR_DEMO_BARRIER_ADDR")
    if comm_id == barrier_addr:
        raise ValueError("communicator 与 Host barrier 必须使用不同端口")
    chain = dict(shape, dtype=dtype, quant_mode=int(quant_mode), fp8_format=fp8_format, block_dim=block_dim, comm_id=comm_id, barrier_addr=barrier_addr,
                 run_mode=run_mode, loops=loops, warmup=warmup,
                 route_mode="balance_shift", seed=2026, kernel_scope="dispatch")
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
        return execute(argv, log / filename, timeout=30, env=env,
                       start_failure="BUILD_FAILED", exit_failure="BUILD_FAILED")

    if config.checkpoint == EMPTY_KERNEL_CHECKPOINT:
        kernel = install / "lib/tilexr/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.o"
        inspect(["readelf", "-h", kernel], "server-dedup-elf.log")

    symbols = inspect(["nm", "-D", "--defined-only", "--no-demangle", "--format=posix", library],
                      "symbols.log")
    # 公共接口采用 extern "C"；匹配完整动态函数符号名，不带参数列表或左括号。
    exported = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in ("T", "W"):
            exported.add(fields[0])
    for name in ("TileXRMoeEpDispatchMemoryServerDedup", "TileXRMoeEpDispatchMemory"):
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
    if config.checkpoint == DISPATCH_CHECKPOINT:
        if manifest.get("server_dedup_contract") != DISPATCH_ABI:
            raise ValueError("需要完整 Dispatch 192B ABI / 单份布局产物，请重新 build_all.sh 和 sync.sh")
        profiling = manifest.get("server_dedup_profiling")
        if type(profiling) is not bool or (check_run_mode and config.chain is not None and
                                           profiling is not (config.chain["run_mode"] == "breakdown")):
            raise ValueError("设备打点开关与运行模式不一致：breakdown 请执行 bash build_all.sh breakdown=1，"
                             "普通模式请执行 bash build_all.sh；成功后执行 bash sync.sh")
        if profiling and manifest.get("server_dedup_profile_schema") != measurements.RECORDS_SCHEMA:
            raise ValueError("需要标准 Dispatch records.v1 打点产物，请执行 bash build_all.sh breakdown=1 和 bash sync.sh")
    if manifest["architecture"] != platform.machine():
        raise ValueError("节点 CPU 架构与构建不一致，不能直接分发二进制")
    if artifact_files(config.install) != manifest["artifacts"]:
        raise ValueError("安装产物缺失或摘要不一致，请重新构建/同步")
    return manifest


def clean_build_directory(config):
    work = config.root / "build/validation"
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
    def command(*args, **kwargs):
        return execute(*args, **kwargs, start_failure="BUILD_FAILED", exit_failure="BUILD_FAILED")

    build_id = uuid.uuid4().hex
    work = clean_build_directory(config)
    require_tools(["cmake", "ctest", "g++", "make"])
    source = inventory(config.root, exclude=(config.hostfile.relative_to(config.root).as_posix(),))
    stage = work / "install"
    log = config.logs / ("build-" + build_id)
    print(f"开始构建 {config.checkpoint}，mode={config.mode}，日志：{log}", flush=True)
    for tool in ("cmake", "g++"):
        command([tool, "--version"], log / (tool + "-version.log"))
    common = ["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_INSTALL_LIBDIR=lib", f"-DCMAKE_INSTALL_PREFIX={stage}"]
    if config.mode == "production":
        command(["cmake", "-S", config.root, "-B", work / "production", *common,
                 "-DTILEXR_BUILD_EP=ON", "-DBUILD_TESTING=OFF", "-DTILEXR_BUILD_TESTS=OFF",
                 f"-DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING={'ON' if config.profiling else 'OFF'}",
                 f"-DTILEXR_EP_BUILD_MEMORY_BOUNDARY_TEST={'ON' if config.checkpoint == MEMORY_BOUNDARY_CHECKPOINT else 'OFF'}"],
                log / "production-configure.log", timeout=600)
        command(["cmake", "--build", work / "production", "-j", config.jobs],
                log / "production-build.log", timeout=3600)
        command(["cmake", "--install", work / "production"], log / "production-install.log", timeout=180)
        for kernel in ("dispatch", "combine", "dispatch_memory", "combine_memory", "dispatch_memory_server_dedup"):
            if not (stage / f"lib/tilexr/kernels/tilexr_ep_{kernel}_kernel.o").is_file():
                raise ValueError(f"缺少 EP kernel：{kernel}")
    host_build = work / "host"
    registration = config.checkpoint == EMPTY_KERNEL_CHECKPOINT
    chain = config.checkpoint in CHAIN_CHECKPOINTS
    command(["cmake", "-S", config.root / "tests/ep", "-B", host_build, *common,
             "-DBUILD_TILEXR_EP_DEMO=OFF", "-DBUILD_TILEXR_EP_PLAN_MULTIRANK_TEST=OFF",
             f"-DBUILD_TILEXR_EP_SERVER_DEDUP_REGISTRATION_TEST={'ON' if registration else 'OFF'}",
             f"-DBUILD_TILEXR_EP_SERVER_DEDUP_CHAIN_TEST={'ON' if chain else 'OFF'}",
             f"-DBUILD_TILEXR_EP_MEMORY_BOUNDARY_TEST={'ON' if config.checkpoint == MEMORY_BOUNDARY_CHECKPOINT else 'OFF'}",
             f"-DTILEXR_INSTALL_PREFIX={stage}",
             f"-DPython3_EXECUTABLE={sys.executable}"], log / "host-configure.log", timeout=600)
    command(["cmake", "--build", host_build, "-j", config.jobs], log / "host-build.log", timeout=1800)
    command(["cmake", "--install", host_build], log / "host-install.log")
    if chain and not (stage / "bin" / CHAIN_PROGRAM).is_file():
        raise ValueError(f"Server Dedup 测试未安装：{CHAIN_PROGRAM}")
    try:
        listing = json.loads(command(["ctest", "--test-dir", host_build, "--show-only=json-v1"],
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
        if len(command) == 2 and command[1].endswith(".py"):
            argv = ["@python", str(Path(command[1]).resolve().relative_to(config.root))]
        elif len(command) == 1:
            name = Path(command[0]).name
            if not (stage / "bin" / name).is_file():
                raise ValueError(f"Host 测试未安装：{name}")
            argv = [str(Path("install/validation/bin") / name)]
        else:
            raise ValueError(f"无法导出测试命令：{command}")
        tests.append({"name": test["name"], "argv": argv})
    names = [test["name"] for test in tests]
    missing = (REQUIRED_TESTS | ({REGISTRATION_TEST} if registration else set())) - set(names)
    if missing or len(names) != len(set(names)):
        raise ValueError(f"必需测试缺失或重复：{sorted(missing)}")
    if config.checkpoint == MEMORY_BOUNDARY_CHECKPOINT:
        # This delivery accepts only the new capacity/access case; no old checkpoint regression.
        tests = []
        for artifact in ("lib/libtilexr-ep-memory-boundary-test.so",
                         "lib/tilexr/kernels/tilexr_ep_memory_boundary_test_kernel.o"):
            if not (stage / artifact).is_file():
                raise ValueError("缺少通信域边界测试产物：" + artifact)
    runtime_check(config, stage, log)
    manifest = {"build_id": build_id, "source": source, "mode": config.mode,
                "checkpoint": config.checkpoint, "architecture": platform.machine(),
                "python": sys.version, "cann": os.environ.get("ASCEND_HOME_PATH", ""),
                "driver": os.environ.get("ASCEND_DRIVER_PATH", ""), "tests": tests,
                "artifacts": artifact_files(stage)}
    if config.checkpoint == DISPATCH_CHECKPOINT:
        manifest["server_dedup_contract"] = dict(DISPATCH_ABI)
        manifest["server_dedup_profiling"] = config.profiling
        if manifest["server_dedup_profiling"]:
            manifest["server_dedup_profile_schema"] = measurements.RECORDS_SCHEMA
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
    if action in ("verify", "node"):
        values = {
            "TILEXR_VALIDATION_CHECKPOINT": config.checkpoint,
            "TILEXR_VALIDATION_BUILD_MODE": config.mode,
            "TILEXR_VALIDATION_RESULT_ROOT": str(config.logs),
        }
        if action == "node":
            values.update(TILEXR_VALIDATION_SERVERS=config.server_count,
                          TILEXR_DIRECT_HOSTS_JSON=json.dumps(config.hosts(), separators=(",", ":")))
            if config.chain:
                for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "quant_mode", "fp8_format",
                            "block_dim", "loops", "warmup", "run_mode", "msprof"):
                    values["TILEXR_VALIDATION_" + key.upper()] = config.chain.get(key)
                values["TILEXR_COMM_ID"] = config.chain["comm_id"]
                values["TILEXR_DEMO_BARRIER_ADDR"] = config.chain["barrier_addr"]
        exports = "; ".join("export " + key + "=" + shlex.quote(str(value))
                             for key, value in values.items() if value is not None)
        command = "TILEXR_DIRECT_CONFIG=" + shlex.quote(exports) + " " + command
    return ["ssh", *SSH_OPTIONS, config.target(host), command]


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
    paths = set(source["files"])
    paths.update([".tilexr-source.json", ".tilexr-validation-root", "install/validation/",
                  str(config.hostfile.relative_to(config.root))])
    if (config.root / "validation.local.sh").exists():
        paths.add("validation.local.sh")
    file_list = log / "files.txt"
    file_list.parent.mkdir(parents=True, exist_ok=True)
    file_list.write_text("\n".join(sorted(paths)) + "\n", encoding="utf-8")

    def send(index, host):
        node_log = log / str(index)
        prepare = '''set -eo pipefail
python3 - "$1" "$2" <<'PY'
import pathlib, shutil, sys
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
install = root / 'install/validation'
if install.is_symlink() or (root / 'install').is_symlink():
    raise SystemExit('installation directory must not be a symlink')
if install.exists():
    shutil.rmtree(install)
local_config = root / 'validation.local.sh'
if sys.argv[2] == 'absent' and local_config.exists():
    local_config.unlink()
root.mkdir(parents=True, exist_ok=True)
PY
'''
        command(["ssh", *SSH_OPTIONS, config.target(host["host"]),
                 "bash -s -- " + shlex.quote(str(config.root)) + " " +
                 ("present" if (config.root / "validation.local.sh").exists() else "absent")],
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


def validate_relay_links(record, config, rank_size):
    """Validate a rank's final output associations before global t matching."""
    links = record.get("relay_links")
    if not isinstance(links, list):
        raise ValueError("缺少 Dispatch relay_links")
    capacity_rows = rank_size * config["bs"] * min(config["topk"], config["moe_expert_num"] // rank_size)
    first_hops = terminals = 0
    routes = set()
    for link in links:
        if not isinstance(link, list) or len(link) != 7 or any(type(value) is not int for value in link):
            raise ValueError("relay_links 必须为 7 整数记录")
        source, token, first_k, relay, index, original_k, count = link
        if (not 0 <= source < rank_size or not 0 <= token < config["bs"] or
                not 0 <= first_k < config["topk"] or not 0 <= original_k < config["topk"] or
                not 0 <= relay < rank_size or not 0 <= index < capacity_rows or
                relay // 8 != record["rank"] // 8):
            raise ValueError("relay_links 身份或 t 越界")
        route = (source, token, original_k)
        if route in routes:
            raise ValueError("relay_links 重复原始 route")
        routes.add(route)
        if first_k == original_k:
            if relay != record["rank"] or not 0 < count < config["topk"]:
                raise ValueError("relay_links 第一跳 owner 或 forward_count 无效")
            first_hops += 1
        else:
            if count != -1:
                raise ValueError("relay_links 终端 forward_count 必须为 -1")
            terminals += 1
    if first_hops != record["first_hop_received"] or terminals != record["relay_received"]:
        raise ValueError("relay_links 与第一跳/终端接收数量不一致")


def validate_dispatch_links(records, config):
    """Match terminal t to its first-hop allocation across all ranks."""
    parents, children, allocated, routes = {}, {}, set(), set()
    for record in records:
        validate_relay_links(record, config, config["rank_size"])
        for source, token, first_k, relay, index, original_k, count in record["relay_links"]:
            route = (source, token, original_k)
            if route in routes:
                raise ValueError("跨 Rank relay_links 重复原始 route")
            routes.add(route)
            key = (source, token, first_k, relay)
            if first_k == original_k:
                if key in parents or (relay, index) in allocated:
                    raise ValueError("第一跳重复或多个 Token 复用同一个 relay/t")
                parents[key] = (index, count)
                allocated.add((relay, index))
            else:
                children.setdefault(key, []).append(index)
    if parents.keys() != children.keys():
        raise ValueError("relay_links 缺少第一跳或终端")
    for key, (index, count) in parents.items():
        if len(children[key]) != count or any(value != index for value in children[key]):
            raise ValueError("终端 t 或转发数量与第一跳不一致")
    if sum(record[kind + "_packets"] for record in records for kind in ("first_hop", "direct", "relay")) != (
            config["rank_size"] * config["bs"] * config["topk"]):
        raise ValueError("完整 Dispatch 物理包总数与原始 route 总数不一致")


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
        invalid_output = (allow_invalid_output and manifest["checkpoint"] == DISPATCH_CHECKPOINT
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
                raise Failure("RESULT_FAILED", "rank 的实际 shape/dtype 与本次配置不一致", rank=mapping["rank"])
        if chain:
            linked = record["chain"]
            loops = 1
            if manifest["checkpoint"] == DISPATCH_CHECKPOINT:
                if chain_config is None:
                    raise Failure("RESULT_FAILED", "缺少本次 Dispatch 配置", rank=mapping["rank"])
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
                            "group_validated", "flags_cleared", "prefix_indices_validated")):
                        raise Failure("RESULT_FAILED", "Dispatch 数据、metadata、group 或轮末清理缺少完整校验", rank=mapping["rank"])
                    try:
                        measurements.validate_dispatch_traffic(record, chain_config)
                        validate_relay_links(record, chain_config, mapping["rank_size"])
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
                for key in ("bs", "h", "topk", "moe_expert_num", "dtype", "quant_mode", "fp8_format", "loops", "warmup", "run_mode"):
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
                env.pop("TILEXR_PROFILING_RECORDS_PATH", None)
                if config.chain["run_mode"] == "breakdown":
                    records_path = log.parent / f"rank-{rank}.records.v1"
                    env.update(TILEXR_PROFILING_CAPTURE_ID=str(uuid.uuid5(uuid.NAMESPACE_URL, "tilexr-validation:" + run_id)),
                               TILEXR_PROFILING_RECORDS_PATH=str(records_path),
                               TILEXR_PROFILING_RECORDS_PER_CORE=str(measurements.capture_capacity(
                                   config.chain["loops"], config.chain["warmup"])))
                if config.chain["run_mode"] == "performance":
                    profile_dir = log.parent / f"msprof-rank-{rank}"
                    argv = [config.chain["msprof"], "--application=" + shlex.join([str(arg) for arg in argv]),
                            "--output=" + str(profile_dir)]
            output = execute(argv, log, timeout=remaining, cwd=config.root, env=env, cancelled=cancelled)
            if config.checkpoint in CHAIN_CHECKPOINTS:
                marker = DISPATCH_MARKER
                if config.checkpoint == MEMORY_BOUNDARY_CHECKPOINT:
                    marker = MEMORY_BOUNDARY_MARKER
                if any(line.startswith(DISPATCH_INVALID_MARKER) for line in output.splitlines()):
                    if any(line.startswith(DISPATCH_MARKER) for line in output.splitlines()):
                        raise Failure("LOG_PARSE_FAILED", "Dispatch 同时报告 PASS 和 INVALID")
                    marker = DISPATCH_INVALID_MARKER
                record = parse_rank_marker(output, marker)
                if not isinstance(record, dict):
                    raise Failure("LOG_PARSE_FAILED", "轮次记录不是 JSON 对象")
                if marker == DISPATCH_INVALID_MARKER and record.get("validation_status") != "FAIL":
                    raise Failure("LOG_PARSE_FAILED", "INVALID marker 缺少输出校验失败状态")
                if marker == DISPATCH_MARKER and record.get("validation_status") == "FAIL":
                    raise Failure("LOG_PARSE_FAILED", "PASS marker 与输出校验失败状态矛盾")
                record["chain"] = parse_rank_marker(output, CHAIN_MARKER)
                if config.checkpoint == DISPATCH_CHECKPOINT:
                    validate_rank_records([record], [mapping], run_id, manifest, config.chain,
                                          check_measurements=False, allow_invalid_output=True)
                    communication_completed = True
                if config.chain["run_mode"] == "performance":
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
    argv = [sys.executable if value == "@python" else str(config.root / value)
            for value in test["argv"]]
    env = None
    if name == REGISTRATION_TEST:
        if device is None or not str(device).isdigit() or not 0 <= int(device) <= 255:
            raise ValueError("设备注册检查缺少有效的 hostfile device ID")
        env = dict(os.environ, TILEXR_EP_SERVER_DEDUP_TEST_DEVICE=str(device))
        env["LD_LIBRARY_PATH"] = str(config.install / "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
    path = log / (name + ".log")
    output = execute(argv, path, timeout=timeout, cwd=config.root, env=env, cancelled=cancelled)
    prefix = "test_tilexr_ep_server_dedup_"
    marker = None
    if name == REGISTRATION_TEST:
        marker, suffix = "EP_SERVER_DEDUP_EMPTY_KERNEL_", " test=registration"
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


def host_validate(config):
    run_id = "host-" + uuid.uuid4().hex
    log = config.logs / run_id
    result = {"run_id": run_id, "host": socket.gethostname(), "checkpoint": config.checkpoint,
              "mode": config.mode, "status": "FAIL", "tests": [], "errors": [],
              "scope": "本机已安装的 Host/Python 回归；不包含设备注册、NPU launch 或通信验证。"}
    started = time.monotonic()
    cancelled = threading.Event()
    handlers = {sig: signal.signal(sig, lambda signum, frame: cancelled.set())
                for sig in (signal.SIGTERM, signal.SIGHUP)}
    current_test = None
    try:
        manifest = verify_build(config, check_run_mode=False)
        result.update(build_id=manifest["build_id"], commit=manifest["source"]["commit"],
                      source_provenance=manifest["source"]["provenance"])
        tests = [test for test in manifest["tests"] if test["name"] != REGISTRATION_TEST]
        names = [test["name"] for test in tests]
        if not names or len(names) != len(set(names)):
            raise ValueError("安装包没有 Host/Python 测试或测试名称重复，请检查构建 manifest")
        result["expected_tests"] = names
        for test in tests:
            current_test = test["name"]
            if cancelled.is_set():
                raise InterruptedError("Host/Python 回归已取消")
            run_validation_test(config, test, log, config.timeout, cancelled=cancelled)
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
    print(f"HOST_VALIDATION_{result['status']} build_id={result.get('build_id', 'unknown')} "
          f"tests={sum(test['status'] == 'PASS' for test in result['tests'])}/{len(result.get('expected_tests', []))}", flush=True)
    print(f"结果：{log / 'summary.json'}\n范围：{result['scope']}", flush=True)
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
    for record in result.get("ranks", []):
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
            if not failures and config.checkpoint == DISPATCH_CHECKPOINT:
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
    elif config.checkpoint in CHAIN_CHECKPOINTS:
        scope = (f"{len(mappings)}P 每 rank 8 GiB communicator（两个 4 GiB window）、"
                  f"{config.chain['warmup']} 轮预热＋{config.chain['loops']} 轮测量（同域连续提交）、"
                  "最后一轮 cnt/cumsum、DIRECT/第一跳/终端完整 route 覆盖、源端输出、"
                  "最终 hidden/scales/assistEx/group、跨 Rank relay/t 关联、Token 级 prefix 编号和轮末 Flag 清理校验；不包含 Combine。")
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
            config.checkpoint == DISPATCH_CHECKPOINT) else sum(
            len(node["reported_result"].get("ranks", [])) for node in nodes if node["status"] == "PASS")
        report.update(rank_size=len(mappings), completed_ranks=completed, rank_mapping=mappings)
        if config.chain is not None:
            report["chain_configuration"] = dict(config.chain)
            if config.checkpoint == DISPATCH_CHECKPOINT:
                records = node_profile_records(nodes, run_id, manifest, mappings, config.chain)
                full_config = dict(config.chain, rank_size=len(mappings))
                kernel_sha = manifest["artifacts"]["lib/tilexr/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.o"]
                if not errors:
                    try:
                        validate_dispatch_links(records, full_config)
                        report["relay_links_validated"] = True
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
                        output_valid=report.get("relay_links_validated", False))
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
    write_json(log / "summary.json", report)
    run_summary.write_result(config.logs, report, log / "summary.json")
    print("\n================================", flush=True)
    print(measurements.render_case_description(report), flush=True)
    if "measurements" in report:
        print(measurements.render_measurements(report["measurements"]), flush=True)
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
    parser.add_argument("action", choices=("env", "build", "sync", "run", "host-validate", "node", "rank", "verify", "cancel"))
    parser.add_argument("arguments", nargs="*")
    args = parser.parse_args()
    try:
        if sys.version_info < (3, 9):
            raise ValueError("七步验证工具要求 Python 3.9 或更新版本")
        if args.action == "cancel":
            # 不解析 hostfile/CANN/运行配置；旧任务清理不能依赖已经改坏的环境。
            class CancelConfig:
                logs = Path(__file__).resolve().parents[2] / "run/validation"
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
            with operation_lock(config, ".operation.lock"):
                sync(config)
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
        if args.action == "run":
            directory = Path(os.environ.get("TILEXR_VALIDATION_RESULT_ROOT", "run/validation"))
            report = dict(status="FAIL", primary_error=records[0], errors=records)
            write_json(directory / "failure.json", report)
            run_summary.write_result(directory, report, directory / "failure.json")
        for record in records:
            report_failure(record)
        return FAILURE_CODES[records[0]["type"]]


if __name__ == "__main__":
    sys.exit(main())
