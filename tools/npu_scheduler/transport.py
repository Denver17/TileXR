from __future__ import annotations

import re
import subprocess
import threading
import time
import uuid
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from .config import HostConfig, SchedulerConfig


@dataclass(frozen=True)
class ProcessInfo:
    device: int
    pid: int
    name: str


@dataclass(frozen=True)
class Observation:
    host: str
    status: str
    observed_at: float
    device_count: int
    covered_devices: tuple[int, ...]
    processes: tuple[ProcessInfo, ...]
    error: str | None = None

    @property
    def idle(self) -> bool:
        return self.status == "idle"


@dataclass(frozen=True)
class RunResult:
    known: bool
    exit_code: int | None
    error: str | None = None


def parse_npu_smi(
    text: str, host: str, expected_devices: int, observed_at: float | None = None
) -> Observation:
    covered: set[int] = set()
    processes: list[ProcessInfo] = []
    in_table = False
    for line in text.splitlines():
        if "Process id" in line and "Process name" in line:
            in_table = True
            continue
        if not in_table:
            continue
        empty = re.search(r"No running processes found in NPU\s+(\d+)", line)
        if empty:
            covered.add(int(empty.group(1)))
            continue
        if "|" not in line:
            continue
        cells = [cell.strip() for cell in line.split("|")[1:-1]]
        if len(cells) < 3:
            continue
        identity, pid = cells[0].split(), cells[1]
        if identity and identity[0].isdigit() and pid.isdigit():
            device = int(identity[0])
            covered.add(device)
            processes.append(ProcessInfo(device, int(pid), cells[2]))
    expected = set(range(expected_devices))
    now = time.time() if observed_at is None else observed_at
    if covered != expected:
        return Observation(
            host,
            "unknown",
            now,
            expected_devices,
            tuple(sorted(covered)),
            tuple(processes),
            f"npu-smi covered devices {sorted(covered)}, expected {sorted(expected)}",
        )
    return Observation(
        host,
        "external_busy" if processes else "idle",
        now,
        expected_devices,
        tuple(sorted(covered)),
        tuple(processes),
    )


class LogFile:
    def __init__(self, path: Path, max_bytes: int):
        self.path, self.max_bytes = path, max_bytes
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self.marker_path = path.with_suffix(path.suffix + ".truncated")

    def append(self, data: bytes) -> None:
        if not data:
            return
        with self._lock:
            current = self.path.stat().st_size if self.path.exists() else 0
            remaining = max(0, self.max_bytes - current)
            with self.path.open("ab") as stream:
                stream.write(data[:remaining])
            if len(data) > remaining:
                self.marker_path.touch(exist_ok=True)

    def read(self, cursor: int, limit: int) -> tuple[str, int, bool]:
        with self._lock:
            if not self.path.exists():
                return "", 0, self.marker_path.exists()
            size = self.path.stat().st_size
            cursor = max(0, min(cursor, size))
            with self.path.open("rb") as stream:
                stream.seek(cursor)
                data = stream.read(limit)
            return (
                data.decode("utf-8", errors="replace"),
                cursor + len(data),
                self.marker_path.exists(),
            )


class SSHTransport:
    def __init__(self, config: SchedulerConfig):
        self.config = config
        self._executor = ThreadPoolExecutor(max_workers=config.max_parallel_ssh)
        self._future_lock = threading.Lock()
        self._probe_futures: dict[str, Future[Observation]] = {}

    def _command(self, host: HostConfig, remote: str) -> list[str]:
        return [
            self.config.ssh_path,
            "-T",
            "-o",
            "LogLevel=ERROR",
            "-o",
            "BatchMode=yes",
            "-o",
            "StrictHostKeyChecking=accept-new",
            "-o",
            "UpdateHostKeys=no",
            "-o",
            f"ConnectTimeout={self.config.connect_timeout_seconds}",
            f"{host.user}@{host.address}",
            remote,
        ]

    def probe(self, host: HostConfig) -> Observation:
        now = time.time()
        try:
            completed = subprocess.run(
                self._command(host, "npu-smi info"),
                stdin=subprocess.DEVNULL,
                capture_output=True,
                check=False,
                timeout=self.config.probe_timeout_seconds,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return Observation(
                host.name, "unknown", now, host.device_count, (), (), str(error)
            )
        if completed.returncode != 0:
            error = completed.stderr.decode("utf-8", errors="replace")[-1000:]
            return Observation(
                host.name,
                "unknown",
                now,
                host.device_count,
                (),
                (),
                f"npu-smi SSH exit {completed.returncode}: {error}",
            )
        return parse_npu_smi(
            completed.stdout.decode("utf-8", errors="replace"),
            host.name,
            host.device_count,
            now,
        )

    def probe_many(self, hosts: tuple[HostConfig, ...]) -> dict[str, Observation]:
        results: dict[str, Observation] = {}
        futures = {host.name: self._probe_future(host) for host in hosts}
        for host in hosts:
            try:
                observation = futures[host.name].result()
            except Exception as error:  # noqa: BLE001 - a failed probe is an unknown observation
                observation = Observation(
                    host.name,
                    "unknown",
                    time.time(),
                    host.device_count,
                    (),
                    (),
                    str(error),
                )
            results[observation.host] = observation
        return results

    def _probe_future(self, host: HostConfig) -> Future[Observation]:
        with self._future_lock:
            existing = self._probe_futures.get(host.name)
            if existing is not None:
                return existing
            future = self._executor.submit(self.probe, host)
            self._probe_futures[host.name] = future
        future.add_done_callback(
            lambda completed, name=host.name: self._forget_probe(name, completed)
        )
        return future

    def _forget_probe(self, host_name: str, completed: Future[Observation]) -> None:
        with self._future_lock:
            if self._probe_futures.get(host_name) is completed:
                self._probe_futures.pop(host_name, None)

    def run(
        self,
        host: HostConfig,
        script_path: str | None,
        script_content: str | None,
        working_directory: str | None,
        log: LogFile,
    ) -> RunResult:
        marker = f"__TILEXR_SCHEDULER_EXIT_{uuid.uuid4().hex}__"
        workdir = working_directory or ""
        delimiter = f"TILEXR_SCRIPT_{uuid.uuid4().hex}"
        body = (
            f"tmp=$(mktemp) || exit 125\ntrap 'rm -f -- \"$tmp\"' EXIT\n"
            f"cat >\"$tmp\" <<'{delimiter}'\n{script_content or ''}\n{delimiter}\n"
            f'bash -- "$tmp" </dev/null\n'
            f'rc=$?\nprintf \'\\n{marker}%s\\n\' "$rc"\n'
        )
        args = ["bash", "-s", "--", workdir]
        wrapper = (
            f'set +e\nif [[ -n "$1" ]] && ! cd -- "$1"; then '
            f"printf '\\n{marker}125\\n'; exit 125; fi\n" + body
        )
        remote = " ".join(_shell_quote(value) for value in args)
        try:
            process = subprocess.Popen(
                self._command(host, remote),
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            assert process.stdin is not None and process.stdout is not None
            process.stdin.write(wrapper.encode("utf-8"))
            process.stdin.close()
            marker_code: int | None = None
            for line in iter(process.stdout.readline, b""):
                decoded = line.decode("utf-8", errors="replace")
                match = re.fullmatch(re.escape(marker) + r"(-?\d+)\r?\n", decoded)
                if match:
                    marker_code = int(match.group(1))
                else:
                    log.append(line)
            return_code = process.wait()
            if marker_code is not None:
                return RunResult(True, marker_code)
            return RunResult(
                False, None, f"SSH exited {return_code} without completion marker"
            )
        except OSError as error:
            return RunResult(False, None, str(error))


def _shell_quote(value: str) -> str:
    return "'" + value.replace("'", "'\"'\"'") + "'"
