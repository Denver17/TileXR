#!/usr/bin/env python3
"""七步 EP 验证：构建、分发、每主机 Host 测试与有界结果汇总。"""

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

from source import content_sha256, snapshot, verify_source


CHECKPOINT = "ep-sdedup-p1-abi-host"
CHECKS = ("protocol", "layout", "host_validation", "public_abi")
SSH_OPTIONS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
               "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"]
REMOTE_SCRIPT = '''set -eo pipefail
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


def execute(argv, log, timeout=180, cwd=None, env=None, stdin=None):
    """写原始日志；超时和异常退出均回收本次命令及其同组子进程。"""
    log = Path(log)
    log.parent.mkdir(parents=True, exist_ok=True)
    print(f"执行：{argv[0]}  日志：{log}", flush=True)
    with log.open("wb") as output:
        process = subprocess.Popen([str(arg) for arg in argv], cwd=cwd, env=env,
                                   stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.communicate(input=stdin.encode() if stdin is not None else None, timeout=timeout)
            code = process.returncode
        finally:
            stop_process(process)
    if code:
        tail = log.read_text(encoding="utf-8", errors="replace")[-4000:]
        raise RuntimeError(f"命令失败（exit={code}）：{argv[0]}\n{tail}\n日志：{log}")
    return log.read_text(encoding="utf-8", errors="replace")


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
        if self.mode not in ("host", "production") or self.checkpoint != CHECKPOINT:
            raise ValueError("当前版本只支持 ep-sdedup-p1-abi-host；不支持设备去重测试")
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
                "hostfile": digest(self.hostfile), "ssh_user": self.user, "timeout": self.timeout,
                "local_config": digest(self.root / "validation.local.sh")
                if (self.root / "validation.local.sh").exists() else None}


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
    finally:
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


def runtime_check(config, install, log, deadline=None):
    if config.mode == "host":
        return
    require_tools(["ldd", "readelf", "nm"])
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(install / "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
    library = install / "lib/libtilexr-ep.so"

    def inspect(argv, filename, env=None):
        remaining = 30 if deadline is None else min(30, deadline - time.monotonic())
        if remaining <= 0:
            raise TimeoutError("生产依赖检查超过节点期限")
        return execute(argv, log / filename, timeout=remaining, env=env)

    symbols = inspect(["nm", "-D", "--defined-only", "-C", library], "symbols.log")
    for name in ("TileXRMoeEpDispatchMemoryServerDedup(", "TileXRMoeEpDispatchMemory("):
        if name not in symbols:
            raise ValueError(f"生产库缺少导出符号：{name}")
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


def verify_build(config, log, deadline=None):
    manifest = read_json(config.install / "manifest.json")
    if manifest["mode"] != config.mode or manifest["checkpoint"] != config.checkpoint:
        raise ValueError("测试配置与构建不一致，请重新执行 build_all.sh 和 sync.sh")
    if manifest["architecture"] != platform.machine():
        raise ValueError("节点 CPU 架构与构建不一致，不能直接分发二进制")
    verify_source(config.root, manifest["source"])
    if artifact_files(config.install) != manifest["artifacts"]:
        raise ValueError("安装产物缺失或摘要不一致，请重新构建/同步")
    runtime_check(config, config.install, log, deadline)
    return manifest


def build(config):
    require_tools(["cmake", "ctest", "g++", "make"])
    source = snapshot(config.root, exclude=(config.hostfile.relative_to(config.root).as_posix(),))
    build_id = uuid.uuid4().hex
    work = config.root / "build/validation" / build_id
    stage = work / "install"
    log = config.logs / ("build-" + build_id)
    print(f"开始构建 {config.checkpoint}，mode={config.mode}，日志：{log}", flush=True)
    for tool in ("cmake", "g++"):
        execute([tool, "--version"], log / (tool + "-version.log"))
    common = ["-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_INSTALL_LIBDIR=lib", f"-DCMAKE_INSTALL_PREFIX={stage}"]
    if config.mode == "production":
        execute(["cmake", "-S", config.root, "-B", work / "production", *common,
                 "-DTILEXR_BUILD_EP=ON", "-DBUILD_TESTING=OFF", "-DTILEXR_BUILD_TESTS=OFF"],
                log / "production-configure.log", timeout=600)
        execute(["cmake", "--build", work / "production", "-j", config.jobs],
                log / "production-build.log", timeout=3600)
        execute(["cmake", "--install", work / "production"], log / "production-install.log", timeout=180)
        for kernel in ("dispatch", "combine", "dispatch_memory", "combine_memory"):
            if not (stage / f"lib/tilexr/kernels/tilexr_ep_{kernel}_kernel.o").is_file():
                raise ValueError(f"缺少 EP kernel：{kernel}")
        runtime_check(config, stage, log)
    host_build = work / "host"
    execute(["cmake", "-S", config.root / "tests/ep", "-B", host_build, *common,
             "-DBUILD_TILEXR_EP_DEMO=OFF", "-DBUILD_TILEXR_EP_PLAN_MULTIRANK_TEST=OFF",
             f"-DPython3_EXECUTABLE={sys.executable}"], log / "host-configure.log", timeout=600)
    execute(["cmake", "--build", host_build, "-j", config.jobs], log / "host-build.log", timeout=1800)
    execute(["cmake", "--install", host_build], log / "host-install.log")
    listing = json.loads(execute(["ctest", "--test-dir", host_build, "--show-only=json-v1"],
                                 log / "tests.json"))
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
    expected = {f"test_tilexr_ep_server_dedup_{check}" for check in CHECKS}
    if len(tests) != 17 or not expected.issubset({test["name"] for test in tests}):
        raise ValueError("必须注册 17 个 EP Host/Python 测试，包括 4 个 Server Dedup 测试")
    verify_source(config.root, source)
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
          f"source_sha256={content_sha256(source['files'])} mode={config.mode} tests=17\n日志：{log}", flush=True)


def remote_argv(config, host, action, *arguments):
    command = "bash -s -- " + " ".join(shlex.quote(str(value)) for value in (config.root, action, *arguments))
    return ["ssh", *SSH_OPTIONS, config.target(host), command]


def sync(config):
    hosts = config.hosts()
    check_coordinator(config, hosts)
    if len(hosts) > 1:
        require_tools(["ssh", "rsync"])
    log = config.logs / ("sync-" + uuid.uuid4().hex)
    manifest = verify_build(config, log / "local")
    receipt = config.logs / "sync.json"
    receipt.unlink(missing_ok=True)
    paths = set(manifest["source"]["files"])
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
        execute(["ssh", *SSH_OPTIONS, config.target(host["host"]),
                 "bash -s -- " + shlex.quote(str(config.root)) + " " +
                 ("present" if (config.root / "validation.local.sh").exists() else "absent")],
                node_log / "prepare.log", stdin=prepare)
        execute(["rsync", "-a", "-r", "--checksum", "--protect-args", "--timeout=30",
                 "--files-from=" + str(file_list), "-e", shlex.join(["ssh", *SSH_OPTIONS]),
                 str(config.root) + "/", config.target(host["host"]) + ":" + str(config.root) + "/"],
                node_log / "transfer.log", timeout=180)
        execute(remote_argv(config, host["host"], "verify", manifest["build_id"]),
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


def node(config, run_id, build_id, execution_budget=None):
    log = config.logs / identifier(run_id)
    log.mkdir(parents=True, exist_ok=True)
    result = {"run_id": run_id, "build_id": build_id, "host": socket.gethostname(),
              "checkpoint": config.checkpoint, "mode": config.mode, "status": "FAIL", "tests": []}
    write_json(log / "pid.json", {"pid": os.getpid(), "start": pid_identity(os.getpid())})

    def interrupted(signum, frame):
        raise InterruptedError(f"节点收到停止信号 {signum}")

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    budget = min(120, config.timeout - 15)
    if execution_budget is not None:
        budget = min(budget, max(1, float(execution_budget)))
    deadline = time.monotonic() + budget
    try:
        if (log / "cancelled").exists():
            raise InterruptedError("控制端已取消本次运行")
        manifest = verify_build(config, log / "preflight", deadline)
        if manifest["build_id"] != build_id:
            raise ValueError("远端 build_id 与本次请求不一致")
        for test in manifest["tests"]:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("节点测试超过执行期限")
            argv = [sys.executable if value == "@python" else str(config.root / value)
                    for value in test["argv"]]
            output = execute(argv, log / (test["name"] + ".log"), timeout=remaining, cwd=config.root)
            prefix = "test_tilexr_ep_server_dedup_"
            if test["name"].startswith(prefix):
                marker = "EP_SERVER_DEDUP_ABI_HOST_PASS test=" + test["name"][len(prefix):]
                if marker not in output.splitlines():
                    raise ValueError(f"缺少本次 PASS marker：{test['name']}")
            result["tests"].append({"name": test["name"], "status": "PASS"})
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = str(error)
    finally:
        result["cleanup"] = "complete"
        write_json(log / "result.json", result)
        (log / "pid.json").unlink(missing_ok=True)
    print(json.dumps(result, ensure_ascii=False), flush=True)
    return 0 if result["status"] == "PASS" else 1


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
    expected = {test["name"] for test in manifest["tests"]}
    actual = [test["name"] for test in result.get("tests", [])]
    if (result.get("run_id") != run_id or result.get("build_id") != manifest["build_id"]
            or result.get("status") != "PASS" or result.get("cleanup") != "complete"
            or result.get("checkpoint") != manifest["checkpoint"] or result.get("mode") != manifest["mode"]
            or len(actual) != len(expected) or set(actual) != expected
            or any(test.get("status") != "PASS" for test in result.get("tests", []))):
        raise ValueError("节点结果失败、缺失、重复或不属于本次运行：" + str(result.get("error", "")))


def run(config):
    deadline = time.monotonic() + config.timeout
    hosts = config.hosts()
    check_coordinator(config, hosts)
    run_id = uuid.uuid4().hex
    log = config.logs / run_id
    manifest = verify_build(config, log / "controller-preflight", deadline - 15)
    receipt = read_json(config.logs / "sync.json")
    if receipt != {"build_id": manifest["build_id"], "configuration": config.signature()}:
        raise ValueError("源码/配置/产物自同步后发生变化，请重新执行 sync.sh")
    print(f"开始测试：hosts={len(hosts)} checkpoint={config.checkpoint} mode={config.mode}\n日志：{log}", flush=True)

    def launch(index, host):
        budget = max(1, min(120, deadline - time.monotonic() - 20))
        if index == 0:
            argv = [sys.executable, Path(__file__), "node", run_id, manifest["build_id"], str(budget)]
            payload = None
        else:
            argv = remote_argv(config, host["host"], "node", run_id, manifest["build_id"], str(budget))
            payload = REMOTE_SCRIPT
        execute(argv, log / f"launch-{index}.log", timeout=max(1, deadline - time.monotonic() - 12), stdin=payload)

    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        futures = {pool.submit(launch, index, host): index for index, host in enumerate(hosts)}
        cancelled = False
        for future in concurrent.futures.as_completed(futures):
            index = futures[future]
            try:
                future.result()
            except Exception as error:
                errors.append({"host": hosts[index]["host"], "stage": "execute", "error": str(error)})
                if not cancelled:
                    cancelled = True
                    # 独立连接通知仍在运行的节点；每个节点自身也有执行期限。
                    def stop(index, host):
                        try:
                            if index == 0:
                                cancel(config, run_id)
                            else:
                                execute(remote_argv(config, host["host"], "cancel", run_id),
                                        log / f"cancel-{index}.log", timeout=10, stdin=REMOTE_SCRIPT)
                        except Exception as failure:
                            return {"host": host["host"], "stage": "cleanup", "error": str(failure)}
                    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as cleanup:
                        for failure in cleanup.map(lambda item: stop(*item), enumerate(hosts)):
                            if failure:
                                errors.append(failure)

    def collect(index, host):
        node_log = log if index == 0 else log / "nodes" / str(index)
        if index:
            node_log.mkdir(parents=True, exist_ok=True)
            execute(["rsync", "-a", "--protect-args", "--timeout=5", "-e", shlex.join(["ssh", *SSH_OPTIONS]),
                     config.target(host["host"]) + ":" + str(log) + "/", str(node_log) + "/"],
                    log / f"collect-{index}.log", timeout=max(1, deadline - time.monotonic()))
        result = read_json(node_log / "result.json")
        validate_result(result, run_id, manifest)
        return {"host": host["host"], "status": "PASS", "tests": len(result["tests"])}

    nodes = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        futures = {pool.submit(collect, index, host): index for index, host in enumerate(hosts)}
        for future in concurrent.futures.as_completed(futures):
            index = futures[future]
            try:
                nodes.append(future.result())
            except Exception as error:
                errors.append({"host": hosts[index]["host"], "stage": "collect/result", "error": str(error)})
    if time.monotonic() > deadline:
        errors.append({"stage": "deadline", "error": "结果汇总超过总期限"})
    report = {"run_id": run_id, "commit": manifest["source"]["commit"], "build_id": manifest["build_id"],
              "source_sha256": content_sha256(manifest["source"]["files"]),
              "checkpoint": config.checkpoint, "mode": config.mode, "status": "FAIL" if errors else "PASS",
              "expected_hosts": hosts, "nodes": nodes, "errors": errors,
              "scope": "ABI/Host tests; no NPU launch or communication correctness"}
    write_json(log / "summary.json", report)
    print(f"VALIDATION_{report['status']} checkpoint={config.checkpoint} mode={config.mode} "
          f"hosts={len(nodes)}/{len(hosts)} tests_per_host=17 commit={report['commit'] or 'unknown'} "
          f"source_sha256={report['source_sha256']}", flush=True)
    for error in errors:
        print(json.dumps(error, ensure_ascii=False), file=sys.stderr)
    print(f"结果：{log / 'summary.json'}\n范围：ABI/Host；未执行 NPU launch 或去重通信。", flush=True)
    return 1 if errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("env", "build", "sync", "run", "node", "verify", "cancel"))
    parser.add_argument("arguments", nargs="*")
    args = parser.parse_args()
    try:
        if sys.version_info < (3, 9):
            raise ValueError("七步验证工具要求 Python 3.9 或更新版本")
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
        elif args.action == "cancel":
            cancel(config, *args.arguments)
        elif args.action == "verify":
            manifest = verify_build(config, config.logs / "sync-verify")
            if manifest["build_id"] != args.arguments[0]:
                raise ValueError("同步后的 build_id 不一致")
            print("VERIFY_PASS")
        return 0
    except Exception as error:
        print(f"VALIDATION_FAIL stage={args.action}: {error}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
