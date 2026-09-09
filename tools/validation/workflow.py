#!/usr/bin/env python3
"""七步 EP 验证：构建、分发、每主机检查与有界结果汇总。"""

import argparse
import concurrent.futures
from contextlib import contextmanager
import hashlib
import json
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
import time
import uuid

from source import inventory


CHECKPOINT = "ep-sdedup-p1-abi-host"
EMPTY_KERNEL_CHECKPOINT = "ep-sdedup-p1-empty-kernel"
REGISTRATION_TEST = "test_tilexr_ep_server_dedup_registration"
CHECKS = ("protocol", "layout", "host_validation", "public_abi")
REQUIRED_TESTS = {f"test_tilexr_ep_server_dedup_{name}" for name in CHECKS} | {
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
source ./set_env.sh
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
            start_failure="LAUNCH_FAILED", exit_failure="EXIT_FAILED", parse_failure="LOG_PARSE_FAILED"):
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
            process.communicate(input=stdin.encode() if stdin is not None else None, timeout=timeout)
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
        if len(fields) > 2 or not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", fields[0]):
            raise ValueError(f"hostfile 第 {number} 行格式错误：host [devices=0,1,...]")
        devices = list(range(8))
        if len(fields) == 2:
            if not re.fullmatch(r"devices=\d+(,\d+)*", fields[1]):
                raise ValueError(f"hostfile 第 {number} 行 devices 格式错误")
            devices = [int(value) for value in fields[1][8:].split(",")]
        if len(set(devices)) != len(devices) or any(device > 255 for device in devices):
            raise ValueError(f"hostfile 第 {number} 行有重复或无效的 device ID")
        if fields[0] in [item["host"] for item in hosts]:
            raise ValueError(f"hostfile 主机重复：{fields[0]}")
        hosts.append({"host": fields[0], "devices": devices})
    if not 1 <= len(hosts) <= 64:
        raise ValueError("hostfile 必须包含 1 至 64 台主机")
    return hosts


class Config:
    def __init__(self):
        self.root = Path(os.environ.get("TILEXR_VALIDATION_ROOT", Path(__file__).resolve().parents[2])).resolve()
        self.mode = os.environ.get("TILEXR_VALIDATION_BUILD_MODE", "production")
        self.checkpoint = os.environ.get("TILEXR_VALIDATION_CHECKPOINT", CHECKPOINT)
        self.hostfile = Path(os.environ.get("TILEXR_VALIDATION_HOSTFILE", self.root / "hostfile"))
        self.user = os.environ.get("TILEXR_VALIDATION_SSH_USER", "")
        self.jobs = int(os.environ.get("TILEXR_VALIDATION_JOBS", "4"))
        self.timeout = int(os.environ.get("TILEXR_VALIDATION_TIMEOUT", "180"))
        if self.mode not in ("host", "production") or self.checkpoint not in (CHECKPOINT, EMPTY_KERNEL_CHECKPOINT):
            raise ValueError("当前版本仅支持 ep-sdedup-p1-abi-host 和 ep-sdedup-p1-empty-kernel")
        if self.checkpoint == EMPTY_KERNEL_CHECKPOINT and self.mode != "production":
            raise ValueError("empty-kernel 必须使用 production 模式验证真实 A5 artifact 和设备注册")
        if not 1 <= self.jobs <= 256 or not 30 <= self.timeout <= 180:
            raise ValueError("JOBS 必须为 1..256，TIMEOUT 必须为 30..180 秒")
        if self.user and not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", self.user):
            raise ValueError("SSH_USER 格式错误")
        self.hostfile = self.hostfile.resolve()
        self.hostfile.relative_to(self.root)  # 同步契约：配置属于当前工作目录。
        self.install = self.root / "install/validation"
        self.logs = self.root / "run/validation"
    def hosts(self):
        try:
            return hosts_from_file(self.hostfile)
        except FileNotFoundError:
            raise ValueError(f"hostfile 不存在：{self.hostfile}；请按 hostfile.example 一次性配置主机清单，"
                             "或检查 validation.local.sh 中的 TILEXR_VALIDATION_HOSTFILE。") from None

    def target(self, host):
        return f"{self.user}@{host}" if self.user else host

    def signature(self):
        return {"mode": self.mode, "checkpoint": self.checkpoint, "hosts": self.hosts(),
                "ssh_user": self.user, "timeout": self.timeout,
                "cann_script": os.environ.get("TILEXR_CANN_ENV_SCRIPT", ""),
                "cann": os.environ.get("ASCEND_HOME_PATH", ""),
                "driver": os.environ.get("ASCEND_DRIVER_PATH", ""),
                "python": sys.executable}


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


def verify_build(config):
    manifest = read_json(config.install / "manifest.json")
    if manifest["mode"] != config.mode or manifest["checkpoint"] != config.checkpoint:
        raise ValueError("测试配置与构建不一致，请重新执行 build_all.sh 和 sync.sh")
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
                 "-DTILEXR_BUILD_EP=ON", "-DBUILD_TESTING=OFF", "-DTILEXR_BUILD_TESTS=OFF"],
                log / "production-configure.log", timeout=600)
        command(["cmake", "--build", work / "production", "-j", config.jobs],
                log / "production-build.log", timeout=3600)
        command(["cmake", "--install", work / "production"], log / "production-install.log", timeout=180)
        for kernel in ("dispatch", "combine", "dispatch_memory", "combine_memory", "dispatch_memory_server_dedup"):
            if not (stage / f"lib/tilexr/kernels/tilexr_ep_{kernel}_kernel.o").is_file():
                raise ValueError(f"缺少 EP kernel：{kernel}")
    host_build = work / "host"
    registration = config.checkpoint == EMPTY_KERNEL_CHECKPOINT
    command(["cmake", "-S", config.root / "tests/ep", "-B", host_build, *common,
             "-DBUILD_TILEXR_EP_DEMO=OFF", "-DBUILD_TILEXR_EP_PLAN_MULTIRANK_TEST=OFF",
             f"-DBUILD_TILEXR_EP_SERVER_DEDUP_REGISTRATION_TEST={'ON' if registration else 'OFF'}",
             f"-DTILEXR_INSTALL_PREFIX={stage}",
             f"-DPython3_EXECUTABLE={sys.executable}"], log / "host-configure.log", timeout=600)
    command(["cmake", "--build", host_build, "-j", config.jobs], log / "host-build.log", timeout=1800)
    command(["cmake", "--install", host_build], log / "host-install.log")
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
    runtime_check(config, stage, log)
    manifest = {"build_id": build_id, "source": source, "mode": config.mode,
                "checkpoint": config.checkpoint, "architecture": platform.machine(),
                "python": sys.version, "cann": os.environ.get("ASCEND_HOME_PATH", ""),
                "driver": os.environ.get("ASCEND_DRIVER_PATH", ""), "tests": tests,
                "artifacts": artifact_files(stage)}
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
    return ["ssh", *SSH_OPTIONS, config.target(host), command]


def sync(config):
    def command(*args, **kwargs):
        return execute(*args, **kwargs, start_failure="SYNC_FAILED", exit_failure="SYNC_FAILED")

    hosts = config.hosts()
    check_coordinator(config, hosts)
    if len(hosts) > 1:
        require_tools(["ssh", "rsync"])
    log = config.logs / ("sync-" + uuid.uuid4().hex)
    manifest = verify_build(config)
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
    write_json(receipt, {"build_id": manifest["build_id"], "configuration": config.signature()})
    print(f"SYNC_PASS hosts={len(hosts)} build_id={manifest['build_id']}\n日志：{log}", flush=True)


def pid_identity(pid):
    try:
        # Linux /proc starttime 防止取消旧运行时命中复用的 PID。
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]
    except FileNotFoundError:
        return None


def node(config, run_id, build_id, execution_budget=None, device=None):
    log = config.logs / identifier(run_id)
    log.mkdir(parents=True, exist_ok=True)
    result = {"run_id": run_id, "build_id": build_id, "host": socket.gethostname(),
              "checkpoint": config.checkpoint, "mode": config.mode, "status": "FAIL", "tests": [], "errors": []}
    write_json(log / "pid.json", {"pid": os.getpid(), "start": pid_identity(os.getpid())})
    print(f"NODE_STARTED run_id={run_id}", flush=True)

    def interrupted(signum, frame):
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
        for test in manifest["tests"]:
            current_test = test["name"]
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("EXIT_FAILED", "节点测试超过执行期限", timeout=True)
            argv = [sys.executable if value == "@python" else str(config.root / value)
                    for value in test["argv"]]
            env = None
            if current_test == REGISTRATION_TEST:
                if device is None or not str(device).isdigit() or not 0 <= int(device) <= 255:
                    raise ValueError("设备注册检查缺少有效的 hostfile device ID")
                env = dict(os.environ, TILEXR_EP_SERVER_DEDUP_TEST_DEVICE=str(device))
                env["LD_LIBRARY_PATH"] = str(config.install / "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
                result["device"] = int(device)
            output = execute(argv, log / (test["name"] + ".log"), timeout=remaining, cwd=config.root, env=env)
            prefix = "test_tilexr_ep_server_dedup_"
            marker = None
            if current_test == REGISTRATION_TEST:
                marker, suffix = "EP_SERVER_DEDUP_EMPTY_KERNEL_", " test=registration"
            elif current_test.startswith(prefix):
                marker, suffix = "EP_SERVER_DEDUP_ABI_HOST_", " test=" + current_test[len(prefix):]
            elif current_test == "test_tilexr_ep_kernel_launch" and config.checkpoint == EMPTY_KERNEL_CHECKPOINT:
                marker, suffix = "EP_SERVER_DEDUP_EMPTY_KERNEL_", " test=launch"
            if marker:
                markers = [line for line in output.splitlines() if line.startswith(marker)]
                if markers == [marker + "FAIL" + suffix]:
                    raise Failure("RESULT_FAILED", "测试完成但结果校验失败", log=str(log / (current_test + ".log")))
                if markers != [marker + "PASS" + suffix]:
                    raise Failure("LOG_PARSE_FAILED", "缺少或无法解析唯一的 PASS/FAIL marker",
                                  log=str(log / (current_test + ".log")))
            result["tests"].append({"name": test["name"], "status": "PASS"})
        result["status"] = "PASS"
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


def validate_result(result, run_id, manifest):
    fields = ("run_id", "build_id", "checkpoint", "mode", "status", "cleanup", "tests")
    if (not isinstance(result, dict) or any(field not in result for field in fields)
            or not isinstance(result["tests"], list)
            or any(not isinstance(test, dict) or not isinstance(test.get("name"), str)
                   or test.get("status") not in ("PASS", "FAIL") for test in result["tests"])
            or result["status"] not in ("PASS", "FAIL")):
        raise Failure("LOG_PARSE_FAILED", "节点结果缺少字段或格式错误")
    expected = {test["name"] for test in manifest["tests"]}
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


def result_errors(result, run_id, manifest):
    try:
        validate_result(result, run_id, manifest)
    except Failure as error:
        return failure_records(error, "RESULT_FAILED")
    return []


def launch_outcome(log, execution_error, run_id, manifest):
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
            errors = result_errors(result, run_id, manifest)
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


def run(config):
    deadline = time.monotonic() + config.timeout
    hosts = config.hosts()
    check_coordinator(config, hosts)
    run_id = uuid.uuid4().hex
    log = config.logs / run_id
    manifest = verify_build(config)
    receipt = read_json(config.logs / "sync.json")
    if receipt != {"build_id": manifest["build_id"], "configuration": config.signature()}:
        raise ValueError("生效配置或构建版本自同步后发生变化，请重新执行 sync.sh")
    print(f"开始测试：hosts={len(hosts)} checkpoint={config.checkpoint} mode={config.mode}\n日志：{log}", flush=True)

    def launch(index, host):
        budget = max(1, min(120, deadline - time.monotonic() - 20))
        arguments = ["node", run_id, manifest["build_id"], str(budget)]
        if config.checkpoint == EMPTY_KERNEL_CHECKPOINT:
            arguments.append(str(host["devices"][0]))
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
        return launch_outcome(launch_log, execution_error, run_id, manifest)

    errors = []
    cleanup_errors = []
    primary_error = None
    launched = {}
    launch_results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        futures = {pool.submit(launch, index, host): index for index, host in enumerate(hosts)}
        cancelled = False
        for future in concurrent.futures.as_completed(futures):
            index = futures[future]
            try:
                launch_results[index], launched[index] = future.result()
            except Exception as error:
                launch_results[index] = None
                launched[index] = [failure_record(error, "LAUNCH_FAILED")]
            if launched[index]:
                if primary_error is None:
                    primary_error = dict(launched[index][0], host=hosts[index]["host"])
                if not cancelled:
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
                execute(["rsync", "-a", "--protect-args", "--timeout=5", "-e", shlex.join(["ssh", *SSH_OPTIONS]),
                         config.target(host["host"]) + ":" + str(log) + "/", str(node_log) + "/"],
                        log / f"collect-{index}.log", timeout=max(1, deadline - time.monotonic()),
                        start_failure="LOG_COLLECT_FAILED", exit_failure="LOG_COLLECT_FAILED")
            result = read_json(node_log / "result.json")
            failures.extend(result_errors(result, run_id, manifest))
        except FileNotFoundError as error:
            # 拉起/异常退出已说明为何没有完成记录，避免追加一个伪解析失败。
            if not failures:
                failures.append(Failure("LOG_PARSE_FAILED", str(error), log=str(node_log / "result.json")).record())
        except (OSError, UnicodeError, ValueError) as error:
            failures.append(Failure("LOG_PARSE_FAILED", str(error), log=str(node_log / "result.json")).record())
        except Failure as error:
            failures.extend(failure_records(error, "LOG_COLLECT_FAILED"))
        unique = {(item["type"], item["message"], item.get("test")): item for item in failures}
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
    report = {"run_id": run_id, "commit": manifest["source"]["commit"], "build_id": manifest["build_id"],
              "source_provenance": manifest["source"]["provenance"],
              "checkpoint": config.checkpoint, "mode": config.mode, "status": "FAIL" if errors else "PASS",
              "expected_hosts": hosts, "nodes": nodes, "primary_error": primary_error, "errors": errors,
              "scope": "ABI/Host tests; no NPU launch or communication correctness"}
    write_json(log / "summary.json", report)
    print(f"VALIDATION_{report['status']} checkpoint={config.checkpoint} mode={config.mode} "
          f"hosts={sum(node['status'] == 'PASS' for node in nodes)}/{len(hosts)} "
          f"tests_per_host={len(manifest['tests'])} commit={report['commit'] or 'unknown'} (unverified)", flush=True)
    for error in errors:
        report_failure(error)
    print(f"结果：{log / 'summary.json'}\n范围：ABI/Host；未执行 NPU launch 或去重通信。", flush=True)
    return FAILURE_CODES[primary_error["type"]] if errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("env", "build", "sync", "run", "node", "verify", "cancel"))
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
        config = Config()
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
        elif args.action == "node":
            with operation_lock(config, "node.lock"):
                return node(config, *args.arguments)
        elif args.action == "verify":
            manifest = verify_build(config)
            if manifest["build_id"] != args.arguments[0]:
                raise ValueError("同步后的 build_id 不一致")
            print("VERIFY_PASS")
        return 0
    except Exception as error:
        kind = {"build": "BUILD_FAILED", "sync": "SYNC_FAILED", "cancel": "CLEANUP_FAILED",
                "node": "LAUNCH_FAILED"}.get(args.action, "CONFIG_FAILED")
        records = failure_records(error, kind, stage=args.action)
        for record in records:
            report_failure(record)
        return FAILURE_CODES[records[0]["type"]]


if __name__ == "__main__":
    sys.exit(main())
